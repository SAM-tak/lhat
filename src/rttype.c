// L^ (lhat) -- the bridge from the checker's types to the machine's. See
// rttype.h for why the two representations stay apart.

#include "rttype.h"


// Converts one of the checker's own LhatType objects into the shape
// lower_type builds from a written annotation. Used only where nothing was
// written at all -- lower_type already has nothing to read there, so this is
// a fallback onto what infer_func (check.c) settled instead, not a second
// opinion on what was written.
//
// A checker type may hold itself (an instance whose member answers one), so
// the structures on the way in are remembered on the C stack. Meeting one
// again is 13.13's Self^ -- the same thing the source would have written
// there -- and how many structures back it was is the hat count.
typedef struct RtSeen {
    const LhatType *type;
    const struct RtSeen *outer;
} RtSeen;

static LhatRuntimeType *rt_from_checked(LhatHeap *heap,
                                        const LhatType *type,
                                        const RtSeen *seen)
{
    if (type == NULL) {
        return NULL;
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
            level++;
        }
    }
    // 13.13 counts structures and nothing else -- a signature is transparent,
    // so only a table joins the chain the hats are counted along.
    RtSeen here = { type, seen };
    if (type->kind == LHAT_TYPE_TABLE) {
        seen = &here;
    }
    switch (type->kind) {
        // 03 の 3.4: inference did not decide this one. Asks nothing of a
        // value, the same as nothing written -- but 14.16 writes it out as
        // UNKNOWN rather than any^, so a signature says which parameter or
        // member is still waiting for an annotation.
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_UNKNOWN);

        case LHAT_TYPE_ANY:
        case LHAT_TYPE_NONE:
            return NULL;  // asks nothing, same as nothing written (13.7)

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

        case LHAT_TYPE_TABLE: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_STRUCTURE);
            if (rt == NULL) {
                return NULL;
            }
            // 14.7改: a definition carries what its instances are, which is
            // what 14.16 writes as the self^{ … } section. It is converted
            // before the definition's own members so that they can point back
            // at it as 13.13's Self^ -- inside a definition that is what the
            // word names, and the definition itself is one step further out.
            RtSeen within = { type->v.table.instance, seen };
            if (type->v.table.instance != NULL) {
                rt->instance =
                    rt_from_checked(heap, type->v.table.instance, seen);
                seen = &within;
            }
            // 14.10: the sequence half first, in position order, the way a
            // written t^{ ... } puts it.
            size_t sequence = 0;
            for (;;) {
                const LhatTypeMember *m =
                    lhat_type_member_at(type, sequence + 1);
                if (m == NULL) {
                    break;
                }
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, m->type, seen))) {
                    return NULL;
                }
                sequence++;
            }
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                bool positional = false;
                for (size_t i = 0; i < sequence; i++) {
                    if (lhat_type_member_at(type, i + 1) == m) {
                        positional = true;
                        break;
                    }
                }
                if (positional) {
                    continue;
                }
                LhatString *name = lhat_string_new(heap, m->name, m->name_length);
                if (name == NULL ||
                    !lhat_type_rt_add_member(rt, name, rt_from_checked(heap, m->type, seen))) {
                    return NULL;
                }
            }
            if (type->v.table.variadic != NULL) {
                rt->variadic = rt_from_checked(heap, type->v.table.variadic, seen);
            }
            lhat_type_rt_sort_members(rt);
            return rt;
        }

        case LHAT_TYPE_FUNC: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_SUBROUTINE);
            if (rt == NULL) {
                return NULL;
            }
            rt->is_function = type->v.func.is_function;
            rt->takes_self = type->v.func.takes_self;
            rt->self_last = type->v.func.self_last;
            rt->closed = type->v.func.closed;  // 15.13
            for (LhatTypeList *p = type->v.func.params; p != NULL; p = p->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, p->type, seen))) {
                    return NULL;
                }
            }
            if (type->v.func.variadic != NULL) {
                rt->variadic = rt_from_checked(heap, type->v.func.variadic, seen);
            }
            // 15.5: what a call answers -- the coroutine where the body
            // yields (13.9). 14.16's typeof^ comes through here, and what a
            // reader wants from a signature is what a call hands back.
            rt->result = rt_from_checked(heap, lhat_type_call_answer(type), seen);
            return rt;
        }

        case LHAT_TYPE_CORO: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_COROUTINE);
            if (rt == NULL) {
                return NULL;
            }
            rt->receive = rt_from_checked(heap, type->v.coroutine.receive, seen);
            rt->produce = rt_from_checked(heap, type->v.coroutine.produce, seen);
            rt->result = rt_from_checked(heap, type->v.coroutine.result, seen);
            rt->is_function = type->v.coroutine.is_function;  // 15.3改
            return rt;
        }

        case LHAT_TYPE_ERROR:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_ERROR);
        // 04 の 2.3 makes ERROR the supertype of every kind; nothing here
        // reaches the runtime LhatErrorKind object a checker-side name would
        // need to name a precise one, so this is the sound coarser answer.
        case LHAT_TYPE_ERROR_SET:
        case LHAT_TYPE_ERROR_KIND:
            return lhat_type_rt_new(heap, LHAT_TYPE_RT_ERROR);

        case LHAT_TYPE_UNION: {
            LhatRuntimeType *rt = lhat_type_rt_new(heap, LHAT_TYPE_RT_UNION);
            if (rt == NULL) {
                return NULL;
            }
            for (LhatTypeList *a = type->v.composite.arms; a != NULL;
                 a = a->next) {
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, a->type, seen))) {
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
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, a->type, seen))) {
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
                if (!lhat_type_rt_add_part(rt, rt_from_checked(heap, a->type, seen))) {
                    return NULL;
                }
            }
            return rt;
        }
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
            return type->v.table.variadic != NULL &&
                   mentions_error(type->v.table.variadic, seen);

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

LhatRuntimeType *lhat_rt_from_checked(LhatHeap *heap, const LhatType *type)
{
    return rt_from_checked(heap, type, NULL);
}

bool lhat_rt_mentions_error(const LhatType *type)
{
    return mentions_error(type, NULL);
}
