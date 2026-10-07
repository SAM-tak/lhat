// L^ (lhat) -- the bridge from the checker's types to the machine's. See
// rttype.h for why the two representations stay apart.

#include "rttype.h"

#include <stdint.h>
#include <string.h>

#include "grow.h"
#include "lhat/port.h"

// Converts resolved types into runtime descriptors, for both written
// annotations and inferred signatures.
//
// A checker type may hold itself (an instance whose member answers one), so
// the structures on the way in are remembered on the C stack. Meeting one
// again is 13.13's Self^ -- the same thing the source would have written
// there -- and how many structures back it was is the hat count.
typedef struct RtSeen {
    const LhatType *type;
    const struct RtSeen *outer;
    LhatRuntimeType *runtime;
    // A definition's instance, in view while its members are converted. Not
    // on the way in: the members were not reached through it.
    bool lent;
} RtSeen;

typedef struct {
    LhatHeap *heap;
    // A missing child descriptor must not silently weaken a runtime type.
    // NULL remains valid for absent/NONE slots; actual failures poison this
    // conversion, whose partial objects remain owned by the caller's heap.
    bool failed;
    LhatRtCache *cache;  // NULL, or one for `heap`
} RtBuild;

static LhatRuntimeType *rt_from_checked(RtBuild *build, const LhatType *type,
                                        const RtSeen *seen);

static LhatRuntimeType *build_rt(RtBuild *build,
                                         const LhatType *type,
                                         const RtSeen *seen)
{
    LhatHeap *heap = build->heap;
    if (type == NULL) {
        return NULL;
    }
    for (const RtSeen *s = seen; s != NULL; s = s->outer) {
        if (s->type == type && s->runtime != NULL) return s->runtime;
    }
    if (type->kind == LHAT_TYPE_ARGUMENT) {
        return rt_from_checked(build, type->v.argument.bound, seen);
    }
    if (type->specialization_base != NULL) {
        LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_APPLIED);
        if (rt == NULL) return NULL;
        rt->result = rt_from_checked(build, type->specialization_base, seen);
        if (rt->result == NULL) return NULL;
        for (const LhatTypeList *a = type->specialization_arguments; a; a = a->next) {
            LhatRuntimeType *argument = rt_from_checked(build, a->type, seen);
            if (argument == NULL || !lhat_type_rt_add_part(rt, argument)) return NULL;
        }
        return rt;
    }
    if (type->kind == LHAT_TYPE_TABLE) {
        unsigned level = 1;
        for (const RtSeen *s = seen; s != NULL; s = s->outer) {
            if (s->type == type) {
                LhatRuntimeType *rt =
                    lhat_type_rt_new(heap, LHAT_TYPE_RT_SELF);
                if (rt != NULL) {
                    rt->levels = level;
                }
                return rt;
            }
            if (s->type->kind == LHAT_TYPE_TABLE) level++;
        }
    }
    // 13.13 counts structures and nothing else -- a signature is transparent,
    // so only a table joins the chain the hats are counted along.
    RtSeen here = { type, seen, NULL };
    if (type->kind == LHAT_TYPE_TABLE) {
        seen = &here;
    }
    switch (type->kind) {
        // 02 の 19 章: identity is the declaration's address; the names ride
        // along for 14.16's writing.
        case LHAT_TYPE_ENUM: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_ENUM);
            if (rt != NULL) {
                rt->enum_decl = type;
                rt->enum_name = lhat_string_new(heap, type->v.error.name,
                                                type->v.error.name_length);
                if (rt->enum_name == NULL) return NULL;
            }
            return rt;
        }
        case LHAT_TYPE_ENUM_MEMBER: {
            LhatRuntimeType *rt =
                lhat_type_rt_new(heap, LHAT_TYPE_RT_ENUM_MEMBER);
            if (rt != NULL) {
                rt->enum_decl = type->v.error.set;
                rt->enum_name = lhat_string_new(heap, type->v.error.name,
                                                type->v.error.name_length);
                if (rt->enum_name == NULL) return NULL;
                size_t index = 0;
                if (type->v.error.set != NULL) {
                    rt->enum_owner_name = lhat_string_new(
                        heap, type->v.error.set->v.error.name,
                        type->v.error.set->v.error.name_length);
                    if (rt->enum_owner_name == NULL) return NULL;
                    size_t at = 0;
                    for (const LhatTypeList *k =
                             type->v.error.set->v.error.kinds;
                         k != NULL; k = k->next) {
                        at++;
                        if (k->type == type) {
                            index = at;
                            break;
                        }
                    }
                }
                rt->enum_member_index = index;
            }
            return rt;
        }
        // 03 の 3.4: inference did not decide this one. Asks nothing of a
        // value, the same as nothing written -- but 14.16 writes it out as
        // UNKNOWN rather than any^, so a signature says which parameter or
        // member is still waiting for an annotation.
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_UNKNOWN);

        // 13.7: asks nothing of a value, which is also what nothing written
        // means -- but a NULL is how 13.9's empty coroutine slot is carried,
        // so any^ is built rather than collapsed. Both still write out as
        // any^; what the kind buys is that a NULL now says only one thing.
        case LHAT_TYPE_ANY:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_ANY);

        case LHAT_TYPE_NONE:
            return NULL;  // 13.2: no value, so there is nothing to ask of one

        case LHAT_TYPE_NIL:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_NIL);
        case LHAT_TYPE_BOOL:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_BOOL);
        case LHAT_TYPE_NUMBER:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_NUMBER);
        case LHAT_TYPE_STRING:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_STRING);

        // 05 の 8.9: identity is the tag, carried across whole.
        case LHAT_TYPE_HOSTVALUE: {
            LhatRuntimeType *rt =
                lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTVALUE);
            if (rt != NULL) {
                rt->hostvalue_tag = type->v.table.hostvalue_tag;
            }
            return rt;
        }
        // And the box under it, told apart by kind alone.
        case LHAT_TYPE_HOSTVALUE_BOX: {
            LhatRuntimeType *rt =
                lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTVALUE_BOX);
            if (rt != NULL) {
                rt->hostvalue_tag = type->v.table.hostvalue_tag;
            }
            return rt;
        }

        case LHAT_TYPE_TABLE: {
            if (type->v.table.is_typeinfo) return lhat_type_rt_new(heap, LHAT_TYPE_RT_TYPEINFO);
            // 05 の 8.8: a registered type is its declaration and nothing
            // else -- the machine asks a value for the tag (object.c's
            // satisfies), never for the members. Converting those would also
            // walk every type they name, and a library whose types name each
            // other (a physics world's bodies, shapes and joints) made
            // millions of nodes that way for a few dozen registrations.
            if (type->v.table.hostdata_tag != NULL) {
                LhatRuntimeType *rt =
                    lhat_type_rt_new(heap, LHAT_TYPE_RT_HOSTDATA);
                if (rt != NULL) {
                    rt->hostdata_tag = type->v.table.hostdata_tag;
                }
                return rt;
            }
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_TABLE);
            if (rt == NULL) {
                return NULL;
            }
            // 14.7改: a definition carries what its instances are, which is
            // what 14.16 writes as the self^{ … } section. It is converted
            // before the definition's own members so that they can point back
            // at it as 13.13's Self^ -- inside a definition that is what the
            // word names, and the definition itself is one step further out.
            RtSeen within = { type->v.table.instance, seen, NULL, true };
            if (type->v.table.instance != NULL) {
                rt->instance =
                    rt_from_checked(build, type->v.table.instance, seen);
                seen = &within;
            }
            // 14.10: the sequence half first, in position order, the way a
            // written t^{ ... } puts it.
            size_t sequence = 0;
            for (;;) {
                const LhatTypeMember *m =
                    lhat_type_member_at(type, sequence);
                if (m == NULL) {
                    break;
                }
                if (!lhat_type_rt_add_part(rt, rt_from_checked(build, m->type, seen))) {
                    return NULL;
                }
                sequence++;
            }
            // 14.7改2 with 05 の 8.8改: the type's own members, then what its
            // links lend. A descriptor is asked what a value may be asked
            // for (13.11), so a name the wrapper answers has to be here --
            // the link is how the type HOLDS it, not a reason to leave it
            // out. lhat_type_find_member decides, so this and a lookup
            // never disagree.
            LhatChain walk = lhat_type_chain(type);
            const LhatType *up;
            while ((up = lhat_chain_next(&walk)) != NULL) {
                // 05 の 8.8改: a host type on the chain is named, not copied.
                // Its members are the host's whole API -- every signature of
                // every method, and twice over for a definition and its
                // instance -- and what a value holds of it is found the way
                // a lookup finds a lent member. So the descriptor keeps the
                // tag and stops here; the tag's own base chain is what
                // satisfies (object.c) walks, as it does for HOSTDATA.
                if (up->v.table.hostdata_tag != NULL) {
                    rt->hostdata_tag = up->v.table.hostdata_tag;
                    break;
                }
                for (const LhatTypeMember *m = up->v.table.members;
                     m != NULL; m = m->next) {
                    if (lhat_type_find_member(type, m->name,
                                              m->name_length) != m) {
                        continue;  // shadowed, or not lent at all
                    }
                    bool positional = false;
                    for (size_t i = 0; i < sequence; i++) {
                        if (lhat_type_member_at(type, i) == m) {
                            positional = true;
                            break;
                        }
                    }
                    if (positional) {
                        continue;
                    }
                    LhatString *name =
                        lhat_string_new(heap, m->name, m->name_length);
                    if (name == NULL ||
                        !lhat_type_rt_add_member(
                            rt, name, rt_from_checked(build, m->type, seen))) {
                        return NULL;
                    }
                }
            }
            if (type->v.table.variadic != NULL) {
                rt->variadic = rt_from_checked(build, type->v.table.variadic, seen);
            }
            if (type->v.table.index_key != NULL) {
                rt->index_key = rt_from_checked(build, type->v.table.index_key, seen);
                rt->index_value = rt_from_checked(build, type->v.table.index_value, seen);
            }
            lhat_type_rt_sort_members(rt);
            return rt;
        }

        case LHAT_TYPE_FUNC: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_SUBROUTINE);
            if (rt == NULL) {
                return NULL;
            }
            RtSeen function = {type, seen, rt};
            seen = &function;
            rt->is_function = type->v.func.is_function;
            rt->takes_self = type->v.func.takes_self;
            rt->self_last = type->v.func.self_last;
            rt->mutable_self = type->v.func.mutable_self;
            rt->answers_fresh = type->v.func.answers_fresh;
            for (LhatTypeList *p = type->v.func.params; p != NULL; p = p->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(build, p->type, seen))) {
                    return NULL;
                }
            }
            if (type->v.func.variadic != NULL) {
                rt->variadic = rt_from_checked(build, type->v.func.variadic, seen);
            }
            // 15.5: what a call answers -- the coroutine where the body
            // yields (13.9). 14.16's typeof^ comes through here, and what a
            // reader wants from a signature is what a call hands back.
            rt->result = rt_from_checked(build, lhat_type_call_answer(type), seen);
            return rt;
        }

        case LHAT_TYPE_CORO: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_COROUTINE);
            if (rt == NULL) {
                return NULL;
            }
            // 13.9: an empty slot stays empty. rt_from_checked answers NULL
            // for a NULL, which is what the writer reads as "left out".
            rt->receive = rt_from_checked(build, type->v.coroutine.receive, seen);
            rt->produce = rt_from_checked(build, type->v.coroutine.produce, seen);
            rt->result = rt_from_checked(build, type->v.coroutine.result, seen);
            rt->endless = type->v.coroutine.endless;
            rt->coroutine_top = type->coroutine_top;
            rt->receive_any = type->receive_any;
            rt->produce_any = type->produce_any;
            rt->result_any = type->result_any;
            rt->kind_any = type->kind_any;
            rt->is_function = type->v.coroutine.is_function;  // 15.3改
            return rt;
        }

        // 04 の 2.3 makes ERROR the top of every kind in its family; nothing
        // here reaches the runtime LhatErrorKind object a checker-side name
        // would need to name a precise one, so the coarser answer is the
        // sound one for a set and a kind alike. 2.7: the family is carried
        // either way, since coarsening across the two tops would not be
        // sound -- they are disjoint.
        case LHAT_TYPE_ERROR:
        case LHAT_TYPE_ERROR_SET:
        case LHAT_TYPE_ERROR_KIND: {
            LhatRuntimeType *rt = lhat_type_rt_new(
                heap, type->v.error.runtime_kind != NULL
                          ? LHAT_TYPE_RT_ERROR_KIND : LHAT_TYPE_RT_ERROR);
            if (rt != NULL) {
                rt->error_local = type->v.error.local;
                rt->error_kind = type->v.error.runtime_kind;
            }
            return rt;
        }

        case LHAT_TYPE_UNION: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_UNION);
            if (rt == NULL) {
                return NULL;
            }
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(build, a->type, seen))) {
                    return NULL;
                }
            }
            return rt;
        }

        case LHAT_TYPE_INTERSECT: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_INTERSECT);
            if (rt == NULL) {
                return NULL;
            }
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(build, a->type, seen))) {
                    return NULL;
                }
            }
            return rt;
        }

        // 13.8改: the positions, in order. Same walk as the two above -- the
        // checker holds them in the same list -- but its own kind, since the
        // order and the count are what a tuple means.
        case LHAT_TYPE_TUPLE: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_TUPLE);
            if (rt == NULL) {
                return NULL;
            }
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(build, a->type, seen))) {
                    return NULL;
                }
            }
            return rt;
        }

        case LHAT_TYPE_KIND_COUNT:
            break;
    }
    return NULL;
}

// 14.16: typeof^ answers the checker's settled type wherever one
// exists -- with one carve-out. An error's identity is the declaration (04
// の 2.4), and the checker's type carries only its NAME; the runtime
// LhatErrorKind object it would take to build the descriptor is not
// reachable from here. The value carries the kind as a pointer read, so an
// operand whose type mentions an error anywhere is answered by the tag
// instruction instead -- which is also what keeps typeof^(e) naming the
// leaf kind rather than the declared union.
//
// 13.13: a Self^ makes the walk come back to a type it has already read, so
// the tables on the way in are remembered on the C stack the way
// rt_from_checked remembers its own -- `seen` is NULL at the outermost
// call. Meeting one again says false: nothing is found here that the first
// visit will not find.
static bool mentions_error(const LhatType *type, const RtSeen *seen)
{
    if (type == NULL) {
        return false;
    }
    if (type->specialization_base != NULL) {
        for (const LhatTypeList *a = type->specialization_arguments; a; a = a->next) {
            if (mentions_error(a->type, seen)) return true;
        }
        return mentions_error(type->specialization_base, seen);
    }
    // A nominal descriptor uses its declaration identity, not its members.
    if (type->kind == LHAT_TYPE_TABLE && type->v.table.nominal) return false;
    if (type->kind == LHAT_TYPE_TABLE) {
        for (const RtSeen *s = seen; s != NULL; s = s->outer) {
            if (s->type == type) {
                return false;
            }
        }
    }
    RtSeen here = { type, seen };
    seen = &here;
    switch (type->kind) {
        case LHAT_TYPE_ERROR:
        case LHAT_TYPE_ERROR_KIND:
        case LHAT_TYPE_ERROR_SET:
            return true;

        case LHAT_TYPE_TABLE:
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                if (mentions_error(m->type, seen)) {
                    return true;
                }
            }
            return mentions_error(type->v.table.variadic, seen) ||
                   mentions_error(type->v.table.index_key, seen) ||
                   mentions_error(type->v.table.index_value, seen);

        case LHAT_TYPE_FUNC:
            for (LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                if (mentions_error(p->type, seen)) {
                    return true;
                }
            }
            // 15.5: the answer rather than the result, since that is what is
            // about to be built -- an error inside the coroutine a yielding
            // body makes has to send this to the instruction just the same.
            return (type->v.func.variadic != NULL &&
                    mentions_error(type->v.func.variadic, seen)) ||
                   mentions_error(lhat_type_call_answer(type), seen);

        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (mentions_error(a->type, seen)) {
                    return true;
                }
            }
            return false;

        case LHAT_TYPE_CORO:
            return mentions_error(type->v.coroutine.receive, seen) ||
                   mentions_error(type->v.coroutine.produce, seen) ||
                   mentions_error(type->v.coroutine.result, seen);

        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Remembering what was converted
// ---------------------------------------------------------------------------
//
// What a conversion makes depends on the structures in view (`seen`) only
// where it meets one of them again: a Self^ counted back to it, or a
// signature pointing at itself. A type none of whose reach is in view
// converts the same as it would with nothing in view -- so that answer is
// kept, and handed back wherever that holds again.
//
// Whether a type in view lies in the reach of the one being converted is a
// question about the checker's type graph, answered through its strongly
// connected components. A structure on the way in reaches the type being
// converted -- that is how the walk got here -- so it lies in that type's
// reach exactly when the two share a component. A definition's instance
// (`lent`) is in view without being on the way in, so for it the question
// is asked of the components themselves: whether the one reaches the other.

typedef struct LhatRtNode {
    const LhatType *type;  // NULL for an empty slot
    LhatRuntimeType *runtime;  // converted with none of its reach in view
    const LhatType *next_in_component;
    size_t index;      // Tarjan's visiting order, from 1; 0 before that
    size_t low;
    size_t component;  // SIZE_MAX while still on the stack
} LhatRtNode;

typedef struct LhatRtReach {
    size_t from;
    size_t to;
    bool used;
    bool reaches;
} LhatRtReach;

static size_t spread(uint64_t key)
{
    key *= 0x9E3779B97F4A7C15ull;
    return (size_t)(key ^ (key >> 29));
}

void lhat_rt_cache_init(LhatRtCache *cache, LhatHeap *heap)
{
    memset(cache, 0, sizeof *cache);
    cache->heap = heap;
}

void lhat_rt_cache_dispose(LhatRtCache *cache)
{
    lhat_free(cache->nodes);
    lhat_free(cache->stack);
    lhat_free(cache->components);
    lhat_free(cache->reach);
    memset(cache, 0, sizeof *cache);
}

static LhatRtNode *node_find(LhatRtCache *cache, const LhatType *type)
{
    if (cache->node_capacity == 0) {
        return NULL;
    }
    size_t mask = cache->node_capacity - 1;
    for (size_t i = spread((uintptr_t)type) & mask;; i = (i + 1) & mask) {
        if (cache->nodes[i].type == type) return &cache->nodes[i];
        if (cache->nodes[i].type == NULL) return NULL;
    }
}

// The node for `type`, made if there was none. Moves every node when the
// table grows, so a pointer taken before this is stale after it.
static LhatRtNode *node_add(LhatRtCache *cache, const LhatType *type)
{
    LhatRtNode *found = node_find(cache, type);
    if (found != NULL) {
        return found;
    }
    if ((cache->node_count + 1) * 2 > cache->node_capacity) {
        size_t grown = cache->node_capacity ? cache->node_capacity * 2 : 256;
        LhatRtNode *nodes = (LhatRtNode *)lhat_calloc(grown, sizeof *nodes);
        if (nodes == NULL) {
            cache->broken = true;
            return NULL;
        }
        for (size_t i = 0; i < cache->node_capacity; i++) {
            if (cache->nodes[i].type == NULL) continue;
            size_t at = spread((uintptr_t)cache->nodes[i].type) & (grown - 1);
            while (nodes[at].type != NULL) at = (at + 1) & (grown - 1);
            nodes[at] = cache->nodes[i];
        }
        lhat_free(cache->nodes);
        cache->nodes = nodes;
        cache->node_capacity = grown;
    }
    size_t mask = cache->node_capacity - 1;
    size_t at = spread((uintptr_t)type) & mask;
    while (cache->nodes[at].type != NULL) at = (at + 1) & mask;
    memset(&cache->nodes[at], 0, sizeof cache->nodes[at]);
    cache->nodes[at].type = type;
    cache->node_count++;
    return &cache->nodes[at];
}

static const LhatType *unwrapped(const LhatType *type)
{
    while (type != NULL && type->kind == LHAT_TYPE_ARGUMENT) type = type->v.argument.bound;
    return type;
}

typedef void (*RtEdge)(void *context, const LhatType *child);

// The types build_rt converts `type`'s descriptor out of -- its edges in the
// graph the components are of. Every edge build_rt follows has to be here;
// one more than it follows only makes the components coarser.
static void each_child(const LhatType *type, RtEdge edge, void *context)
{
#define RT_EDGE(t) do { const LhatType *child_ = unwrapped(t); \
                        if (child_ != NULL) edge(context, child_); } while (0)
    if (type->specialization_base != NULL) {
        for (const LhatTypeList *a = type->specialization_arguments; a; a = a->next) {
            RT_EDGE(a->type);
        }
        RT_EDGE(type->specialization_base);
        return;
    }
    switch (type->kind) {
        case LHAT_TYPE_TABLE: {
            if (type->v.table.is_typeinfo || type->v.table.hostdata_tag != NULL) {
                return;
            }
            RT_EDGE(type->v.table.instance);
            LhatChain walk = lhat_type_chain(type);
            const LhatType *up;
            while ((up = lhat_chain_next(&walk)) != NULL &&
                   up->v.table.hostdata_tag == NULL) {
                for (const LhatTypeMember *m = up->v.table.members; m != NULL;
                     m = m->next) {
                    RT_EDGE(m->type);
                }
            }
            RT_EDGE(type->v.table.variadic);
            RT_EDGE(type->v.table.index_key);
            RT_EDGE(type->v.table.index_value);
            return;
        }
        case LHAT_TYPE_FUNC:
            for (const LhatTypeList *p = type->v.func.params; p != NULL; p = p->next) {
                RT_EDGE(p->type);
            }
            RT_EDGE(type->v.func.variadic);
            RT_EDGE(lhat_type_call_answer(type));
            return;
        case LHAT_TYPE_CORO:
            RT_EDGE(type->v.coroutine.receive);
            RT_EDGE(type->v.coroutine.produce);
            RT_EDGE(type->v.coroutine.result);
            return;
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
        case LHAT_TYPE_TUPLE:
            for (const LhatTypeList *a = type->v.composite.arms; a != NULL; a = a->next) {
                RT_EDGE(a->type);
            }
            return;
        default:
            return;
    }
#undef RT_EDGE
}

typedef struct {
    LhatRtCache *cache;
    const LhatType *from;
} RtTarjan;

static void strongconnect(LhatRtCache *cache, const LhatType *type);

static void tarjan_edge(void *context, const LhatType *child)
{
    RtTarjan *at = (RtTarjan *)context;
    LhatRtCache *cache = at->cache;
    LhatRtNode *to = node_find(cache, child);
    size_t low;
    if (to == NULL || to->index == 0) {
        strongconnect(cache, child);
        to = node_find(cache, child);
        if (to == NULL) return;
        low = to->low;
    } else if (to->component == SIZE_MAX) {
        low = to->index;
    } else {
        return;
    }
    LhatRtNode *from = node_find(cache, at->from);
    if (from != NULL && low < from->low) from->low = low;
}

static void strongconnect(LhatRtCache *cache, const LhatType *type)
{
    LhatRtNode *node = node_add(cache, type);
    if (node == NULL) {
        return;
    }
    node->index = node->low = ++cache->next_index;
    node->component = SIZE_MAX;
    LHAT_GROW(cache->stack, cache->stack_count, cache->stack_capacity, 64,
              { cache->broken = true; return; });
    cache->stack[cache->stack_count++] = type;
    RtTarjan at = {cache, type};
    each_child(type, tarjan_edge, &at);
    node = node_find(cache, type);
    if (cache->broken || node->low != node->index) {
        return;
    }
    LHAT_GROW(cache->components, cache->component_count,
              cache->component_capacity, 64, { cache->broken = true; return; });
    size_t component = cache->component_count++;
    const LhatType *member;
    cache->components[component] = NULL;
    do {
        member = cache->stack[--cache->stack_count];
        LhatRtNode *m = node_find(cache, member);
        m->component = component;
        m->next_in_component = cache->components[component];
        cache->components[component] = member;
    } while (member != type);
}

static size_t component_of(LhatRtCache *cache, const LhatType *type)
{
    LhatRtNode *node = node_find(cache, type);
    if (node == NULL || node->index == 0) {
        strongconnect(cache, type);
        node = node_find(cache, type);
    }
    return cache->broken || node == NULL ? SIZE_MAX : node->component;
}

static LhatRtReach *reach_slot(LhatRtCache *cache, size_t from, size_t to)
{
    if ((cache->reach_count + 1) * 2 > cache->reach_capacity) {
        size_t grown = cache->reach_capacity ? cache->reach_capacity * 2 : 64;
        LhatRtReach *reach = (LhatRtReach *)lhat_calloc(grown, sizeof *reach);
        if (reach == NULL) {
            cache->broken = true;
            return NULL;
        }
        for (size_t i = 0; i < cache->reach_capacity; i++) {
            if (!cache->reach[i].used) continue;
            size_t at = spread(((uint64_t)cache->reach[i].from << 32) ^
                               cache->reach[i].to) & (grown - 1);
            while (reach[at].used) at = (at + 1) & (grown - 1);
            reach[at] = cache->reach[i];
        }
        lhat_free(cache->reach);
        cache->reach = reach;
        cache->reach_capacity = grown;
    }
    size_t mask = cache->reach_capacity - 1;
    size_t at = spread(((uint64_t)from << 32) ^ to) & mask;
    while (cache->reach[at].used &&
           (cache->reach[at].from != from || cache->reach[at].to != to)) {
        at = (at + 1) & mask;
    }
    return &cache->reach[at];
}

typedef struct {
    LhatRtCache *cache;
    size_t from;
    size_t to;
    bool found;
} RtReachWalk;

static bool component_reaches(LhatRtCache *cache, size_t from, size_t to);

static void reach_edge(void *context, const LhatType *child)
{
    RtReachWalk *walk = (RtReachWalk *)context;
    if (walk->found) return;
    LhatRtNode *node = node_find(walk->cache, child);
    if (node == NULL || node->component == SIZE_MAX) {
        walk->found = true;  // not known: answer the safe way
    } else if (node->component != walk->from) {
        walk->found = component_reaches(walk->cache, node->component, walk->to);
    }
}

// Over the components, which never form a cycle -- so a walk down them ends,
// and what one answered stands for the rest of the compile.
static bool component_reaches(LhatRtCache *cache, size_t from, size_t to)
{
    if (from == to) {
        return true;
    }
    LhatRtReach *slot = reach_slot(cache, from, to);
    if (slot == NULL) {
        return true;
    }
    if (slot->used) {
        return slot->reaches;
    }
    RtReachWalk walk = {cache, from, to, false};
    for (const LhatType *t = cache->components[from]; t != NULL && !walk.found;
         t = node_find(cache, t)->next_in_component) {
        each_child(t, reach_edge, &walk);
    }
    slot = reach_slot(cache, from, to);
    if (slot == NULL) {
        return true;
    }
    if (!slot->used) {
        slot->used = true;
        cache->reach_count++;
    }
    slot->from = from;
    slot->to = to;
    slot->reaches = walk.found;
    return walk.found;
}

// Whether nothing in view lies in `type`'s reach -- so converting it here
// makes what converting it with nothing in view makes.
static bool converts_alone(LhatRtCache *cache, const LhatType *type,
                           const RtSeen *seen)
{
    if (seen == NULL) {
        return !cache->broken;
    }
    size_t own = component_of(cache, type);
    for (const RtSeen *s = seen; s != NULL && own != SIZE_MAX; s = s->outer) {
        size_t other = component_of(cache, s->type);
        if (other == SIZE_MAX ||
            (s->lent ? component_reaches(cache, own, other) : other == own)) {
            return false;
        }
    }
    return own != SIZE_MAX && !cache->broken;
}

static LhatRuntimeType *rt_from_checked(RtBuild *build, const LhatType *type,
                                        const RtSeen *seen)
{
    if (build->failed) return NULL;
    type = unwrapped(type);
    LhatRtCache *cache = build->cache;
    bool remember = cache != NULL && type != NULL &&
                    converts_alone(cache, type, seen);
    LhatRtNode *node = remember ? node_find(cache, type) : NULL;
    if (node != NULL && node->runtime != NULL) {
        return node->runtime;
    }
    LhatRuntimeType *rt = build_rt(build, type, seen);
    if (rt == NULL && type != NULL && type->kind != LHAT_TYPE_NONE) build->failed = true;
    if (build->failed) return NULL;
    node = remember ? node_add(cache, type) : NULL;
    if (node != NULL) {
        node->runtime = rt;
    }
    return rt;
}

LhatRuntimeType *lhat_rt_from_checked(LhatHeap *heap, const LhatType *type,
                                      LhatRtCache *cache)
{
    RtBuild build = {heap, false,
                     cache != NULL && cache->heap == heap ? cache : NULL};
    return rt_from_checked(&build, type, NULL);
}

bool lhat_rt_mentions_error(const LhatType *type)
{
    return mentions_error(type, NULL);
}
