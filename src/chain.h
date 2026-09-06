// Finite, allocation-free iteration over a singly linked chain, including rings.
#ifndef LHAT_CHAIN_H
#define LHAT_CHAIN_H

#include <stddef.h>
#include <stdint.h>

typedef const void *(*LhatChainNext)(const void *);

typedef struct {
    const void *start, *current, *slow, *fast;
    LhatChainNext next;
    size_t count, limit;
} LhatChain;

static inline LhatChain lhat_chain(const void *start, LhatChainNext next)
{
    LhatChain walk = {start, start, start, start, next, 0, SIZE_MAX};
    return walk;
}

// Floyd's lookahead meets before the iterator would repeat a node. Once it
// meets, count the prefix and ring so enumeration visits each node only once.
static inline const void *lhat_chain_next(LhatChain *walk)
{
    const void *at = walk->current;
    if (at == NULL || walk->count == walk->limit) {
        return NULL;
    }
    walk->current = walk->next(at);
    walk->count++;
    if (walk->fast != NULL && walk->limit == SIZE_MAX) {
        walk->slow = walk->next(walk->slow);
        walk->fast = walk->next(walk->fast);
        if (walk->fast != NULL) {
            walk->fast = walk->next(walk->fast);
        }
        if (walk->fast != NULL && walk->slow == walk->fast) {
            size_t prefix = 0, ring = 1;
            const void *entry = walk->start;
            const void *meet = walk->slow;
            while (entry != meet) {
                entry = walk->next(entry);
                meet = walk->next(meet);
                prefix++;
            }
            for (meet = walk->next(entry); meet != entry;
                 meet = walk->next(meet)) {
                ring++;
            }
            walk->limit = prefix + ring;
        }
    }
    return at;
}

#endif
