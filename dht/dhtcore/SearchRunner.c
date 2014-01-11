/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "dht/Address.h"
#include "dht/dhtcore/SearchRunner.h"
#include "dht/dhtcore/SearchStore.h"
#include "dht/dhtcore/RumorMill.h"
#include "dht/dhtcore/RouterModule.h"
#include "dht/dhtcore/NodeStore.h"
#include "dht/dhtcore/NodeList.h"
#include "dht/dhtcore/VersionList.h"
#include "dht/CJDHTConstants.h"
#include "switch/LabelSplicer.h"
#include "util/Identity.h"
#include "util/Bits.h"
#include "util/log/Log.h"
#include "util/events/EventBase.h"
#include "util/events/Timeout.h"
#include "util/version/Version.h"


/** The maximum number of requests to make before calling a search failed. */
#define MAX_REQUESTS_PER_SEARCH 8


struct SearchRunner_pvt
{
    struct SearchRunner pub;
    struct SearchStore* searchStore;
    struct NodeStore* nodeStore;
    struct Log* logger;
    struct EventBase* eventBase;
    struct RouterModule* router;
    struct RumorMill* rumorMill;
    uint8_t myAddress[16];

    /** Number of concurrent searches in operation. */
    int searches;

    /** Maximum number of concurrent searches allowed. */
    int maxConcurrentSearches;

    /** Beginning of a linked list of searches. */
    struct SearchRunner_Search* firstSearch;

    Identity
};


/**
 * A context for the internals of a search.
 */
struct SearchRunner_Search;
struct SearchRunner_Search
{
    struct RouterModule_Promise pub;

    /** The router module carrying out the search. */
    struct SearchRunner_pvt* const runner;

    /** The number of requests which have been sent out so far for this search. */
    uint32_t totalRequests;

    /** The address which we are searching for. */
    struct Address target;

    /** String form of the 16 byte ipv6 address. */
    String* targetStr;

    /**
     * The SearchStore_Search structure for this search,
     * used to keep track of which nodes are participating.
     */
    struct SearchStore_Search* search;

    /** The last node sent a search request. */
    struct Address lastNodeAsked;

    /**
     * The timeout if this timeout is hit then the search will continue
     * but the node will still be allowed to respond and it will be counted as a pong.
     */
    struct Timeout* continueSearchTimeout;

    /** Next search in the linked list. */
    struct SearchRunner_Search* nextSearch;

    /** Self pointer for this search so that the search can be removed from the linked list. */
    struct SearchRunner_Search** thisSearch;

    Identity
};

/**
 * Spot a duplicate entry in a node list.
 * If a router sends a response containing duplicate entries,
 * only the last (best) entry should be accepted.
 *
 * @param nodes the list of nodes.
 * @param index the index of the entry to check for being a duplicate.
 * @return true if duplicate, otherwise false.
 */
static inline bool isDuplicateEntry(String* nodes, uint32_t index)
{
    for (uint32_t i = index; i < nodes->len; i += Address_SERIALIZED_SIZE) {
        if (i == index) {
            continue;
        }
        if (Bits_memcmp(&nodes->bytes[index], &nodes->bytes[i], Address_KEY_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

static void searchStep(struct SearchRunner_Search* search);

static void searchReplyCallback(struct RouterModule_Promise* promise,
                                uint32_t lagMilliseconds,
                                struct Node_Two* fromNode,
                                Dict* result)
{
    struct SearchRunner_Search* search =
        Identity_cast((struct SearchRunner_Search*)promise->userData);

    String* nodes = Dict_getString(result, CJDHTConstants_NODES);

    if (nodes && (nodes->len == 0 || nodes->len % Address_SERIALIZED_SIZE != 0)) {
        Log_debug(search->runner->logger, "Dropping unrecognized reply");
        return;
    }

    struct VersionList* versions = NULL;
    String* versionsStr = Dict_getString(result, CJDHTConstants_NODE_PROTOCOLS);
    if (versionsStr) {
        versions = VersionList_parse(versionsStr, promise->alloc);
        #ifdef Version_1_COMPAT
            // Version 1 lies about the versions of other nodes, assume they're all v1.
            if (fromNode->version < 2) {
                for (int i = 0; i < (int)versions->length; i++) {
                    versions->versions[i] = 1;
                }
            }
        #endif
    }
    if (!versions || versions->length != (nodes->len / Address_SERIALIZED_SIZE)) {
        Log_debug(search->runner->logger, "Reply with missing or invalid versions");
        return;
    }

    for (uint32_t i = 0; nodes && i < nodes->len; i += Address_SERIALIZED_SIZE) {
        if (isDuplicateEntry(nodes, i)) {
            continue;
        }
        struct Address addr = { .path = 0 };
        Address_parse(&addr, (uint8_t*) &nodes->bytes[i]);
        addr.protocolVersion = versions->versions[i / Address_SERIALIZED_SIZE];

        // calculate the ipv6
        Address_getPrefix(&addr);

        // We need to splice the given address on to the end of the
        // address of the node which gave it to us.
        addr.path = LabelSplicer_splice(addr.path, fromNode->address.path);

        /*#ifdef Log_DEBUG
            uint8_t splicedAddr[60];
            Address_print(splicedAddr, &addr);
            Log_debug(search->runner->logger, "Spliced Address is now:\n    %s", splicedAddr);
        #endif*/

        if (addr.path == UINT64_MAX) {
            Log_debug(search->runner->logger, "Dropping node because route could not be spliced");
            continue;
        }

        #ifdef Log_DEBUG
            uint8_t printedAddr[60];
            Address_print(printedAddr, &addr);
            Log_debug(search->runner->logger, "discovered node [%s]", printedAddr);
        #endif

        if (!Bits_memcmp(search->runner->myAddress, addr.ip6.bytes, 16)) {
            // Any path which loops back through us is necessarily a dead route.
            Log_debug(search->runner->logger, "Detected a loop-route");
            NodeStore_brokenPath(addr.path, search->runner->nodeStore);
            continue;
        }

        Address_getPrefix(&addr);
        if (!AddressCalc_validAddress(addr.ip6.bytes)) {
            Log_debug(search->runner->logger, "Was told garbage.\n");
            // This should never happen, badnode.
            break;
        }

        // Nodes we are told about are inserted with 0 reach and assumed version 1.
        struct Node_Two* nn = NodeStore_nodeForPath(search->runner->nodeStore, addr.path);
        if (!nn || Bits_memcmp(nn->address.key, addr.key, 32)) {
            RumorMill_addNode(search->runner->rumorMill, &addr);
        }

        if (Address_closest(&search->target, &addr, &fromNode->address) >= 0) {
            // Too much noise.
            //Log_debug(search->runner->logger, "Answer was further from the target than us.\n");
            continue;
        }

        if (search->lastNodeAsked.path != fromNode->address.path) {
            continue;
        }

        struct Node_Two* n = NodeStore_getBest(&addr, search->runner->nodeStore);
        SearchStore_addNodeToSearch((n) ? &n->address : &addr, search->search);
    }

    if (search->lastNodeAsked.path != fromNode->address.path) {
        //Log_debug(search->runner->logger, "Late answer in search");
        return;
    }
}

static void searchCallback(struct RouterModule_Promise* promise,
                           uint32_t lagMilliseconds,
                           struct Node_Two* fromNode,
                           Dict* result)
{
    struct SearchRunner_Search* search =
        Identity_cast((struct SearchRunner_Search*)promise->userData);

    if (fromNode) {
        searchReplyCallback(promise, lagMilliseconds, fromNode, result);
    }

    if (search->pub.callback) {
        search->pub.callback(&search->pub, lagMilliseconds, fromNode, result);
    }
    searchStep(search);
}

/**
 * Send a search request to the next node in this search.
 * This is called whenever a response comes in or after the global mean response time passes.
 */
static void searchStep(struct SearchRunner_Search* search)
{
    struct SearchRunner_pvt* ctx = Identity_cast((struct SearchRunner_pvt*)search->runner);

    struct Node_Two* node;
    struct SearchStore_Node* nextSearchNode;
    do {
        nextSearchNode = SearchStore_getNextNode(search->search);

        // If the number of requests sent has exceeded the max search requests, let's stop there.
        if (search->totalRequests >= MAX_REQUESTS_PER_SEARCH || nextSearchNode == NULL) {
            if (search->pub.callback) {
                search->pub.callback(&search->pub, 0, NULL, NULL);
            }
            Allocator_free(search->pub.alloc);
            return;
        }

        node = NodeStore_getBest(&nextSearchNode->address, ctx->nodeStore);

    } while (!node || Bits_memcmp(node->address.ip6.bytes, nextSearchNode->address.ip6.bytes, 16));

    Bits_memcpyConst(&search->lastNodeAsked, &node->address, sizeof(struct Address));

    struct RouterModule_Promise* rp =
        RouterModule_newMessage(&node->address, 0, ctx->router, search->pub.alloc);

    Dict* message = Dict_new(rp->alloc);
    Dict_putString(message, CJDHTConstants_QUERY, CJDHTConstants_QUERY_FN, rp->alloc);
    Dict_putString(message, CJDHTConstants_TARGET, search->targetStr, rp->alloc);

    rp->userData = search;
    rp->callback = searchCallback;

    RouterModule_sendMessage(rp, message);

    search->totalRequests++;
}

// Triggered by a search timeout (the message may still come back and will be treated as a ping)
static void searchNextNode(void* vsearch)
{
    struct SearchRunner_Search* search = Identity_cast((struct SearchRunner_Search*) vsearch);

    // Timeout for trying the next node.
    Timeout_resetTimeout(search->continueSearchTimeout,
                         RouterModule_searchTimeoutMilliseconds(search->runner->router));

    searchStep(search);
}

static int searchOnFree(struct Allocator_OnFreeJob* job)
{
    struct SearchRunner_Search* search =
        Identity_cast((struct SearchRunner_Search*)job->userData);

    *search->thisSearch = search->nextSearch;
    if (search->nextSearch) {
        search->nextSearch->thisSearch = search->thisSearch;
    }
    Assert_true(search->runner->searches > 0);
    search->runner->searches--;
    return 0;
}

struct SearchRunner_SearchData* SearchRunner_showActiveSearch(struct SearchRunner* searchRunner,
                                                              int number,
                                                              struct Allocator* alloc)
{
    struct SearchRunner_pvt* runner = Identity_cast((struct SearchRunner_pvt*)searchRunner);
    struct SearchRunner_Search* search = runner->firstSearch;
    while (search && number > 0) {
        search = search->nextSearch;
        number--;
    }

    struct SearchRunner_SearchData* out =
        Allocator_calloc(alloc, sizeof(struct SearchRunner_SearchData), 1);

    if (search) {
        Bits_memcpyConst(out->target, &search->target.ip6.bytes, 16);
        Bits_memcpyConst(&out->lastNodeAsked, &search->lastNodeAsked, sizeof(struct Address));
        out->totalRequests = search->totalRequests;
    }
    out->activeSearches = runner->searches;

    return out;
}

struct RouterModule_Promise* SearchRunner_search(uint8_t target[16],
                                                 struct SearchRunner* searchRunner,
                                                 struct Allocator* allocator)
{
    struct SearchRunner_pvt* runner = Identity_cast((struct SearchRunner_pvt*)searchRunner);

    if (runner->searches > runner->maxConcurrentSearches) {
        Log_debug(runner->logger, "Skipping search because there are already [%d] searches active",
                  runner->searches);
        return NULL;
    }

    struct Allocator* alloc = Allocator_child(allocator);
    struct SearchStore_Search* sss = SearchStore_newSearch(target, runner->searchStore, alloc);

    struct Address targetAddr = { .path = 0 };
    Bits_memcpyConst(targetAddr.ip6.bytes, target, Address_SEARCH_TARGET_SIZE);

    struct NodeList* nodes =
        NodeStore_getClosestNodes(runner->nodeStore,
                                  &targetAddr,
                                  NULL,
                                  RouterModule_K,
                                  Version_CURRENT_PROTOCOL,
                                  alloc);

    if (nodes->size == 0) {
        Log_debug(runner->logger, "No nodes available for beginning search");
        return NULL;
    }
    Log_debug(runner->logger, "Beginning search");

    for (int i = 0; i < (int)nodes->size; i++) {
        SearchStore_addNodeToSearch(&nodes->nodes[i]->address, sss);
    }

    struct SearchRunner_Search* search = Allocator_clone(alloc, (&(struct SearchRunner_Search) {
        .pub = {
            .alloc = alloc
        },
        .runner = runner,
        .search = sss
    }));
    Identity_set(search);
    runner->searches++;
    Allocator_onFree(alloc, searchOnFree, search);
    Bits_memcpyConst(&search->target, &targetAddr, sizeof(struct Address));

    if (runner->firstSearch) {
        search->nextSearch = runner->firstSearch;
        runner->firstSearch->thisSearch = &search->nextSearch;
    }
    runner->firstSearch = search;
    search->thisSearch = &runner->firstSearch;

    search->targetStr = String_newBinary((char*)targetAddr.ip6.bytes, 16, alloc);

    // Trigger the searchNextNode() immedietly but asynchronously.
    search->continueSearchTimeout =
        Timeout_setTimeout(searchNextNode, search, 0, runner->eventBase, alloc);

    return &search->pub;
}

struct SearchRunner* SearchRunner_new(struct NodeStore* nodeStore,
                                      struct Log* logger,
                                      struct EventBase* base,
                                      struct RouterModule* module,
                                      uint8_t myAddress[16],
                                      struct RumorMill* rumorMill,
                                      struct Allocator* alloc)
{
    struct SearchRunner_pvt* out = Allocator_clone(alloc, (&(struct SearchRunner_pvt) {
        .nodeStore = nodeStore,
        .logger = logger,
        .eventBase = base,
        .router = module,
        .rumorMill = rumorMill,
        .maxConcurrentSearches = SearchRunner_DEFAULT_MAX_CONCURRENT_SEARCHES
    }));
    out->searchStore = SearchStore_new(alloc, logger);
    Bits_memcpyConst(out->myAddress, myAddress, 16);
    Identity_set(out);

    return &out->pub;
}
