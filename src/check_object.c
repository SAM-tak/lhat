// L^ (lhat) -- the type checking stage: the object model -- table literals and def^ ([syntax.obj]).

#include "check_internal.h"
#include "instantiation_internal.h"
#include "rttype.h"

static bool declares_self(Checker *c, const LhatNode *value);

LhatType *chk_infer_table(Checker *c, const LhatNode *node)
{
    const LhatNode *items = node->kind == LHAT_NODE_NEW
                                ? node->v.named.members : node->v.list.items;
    LhatType *table = lhat_type_table(c->result->types);
    table->v.table.entries_known = true;

    // [syntax.obj.self]: a method written in the literal reaches the table through its
    // self^ receiver, the same receiver a def^'s method takes -- [syntax.stmt.old-world] took
    // the bound name out of the initialiser, and this is the door that
    // stays. The named signatures go in first, annotations only, so a body
    // walked below can call a method declared after it. (The full fixpoint
    // stays def^'s, [syntax.obj.members-order]: a member whose type only its body knows is not
    // seeded here, and reading it ahead is what a def^ is for.)
    for (const LhatNode *entry = items; entry != NULL;
         entry = entry->next) {
        const char *seeded = NULL;
        size_t seeded_length = 0;
        if (entry->v.entry.computed || entry->v.entry.value == NULL ||
            entry->v.entry.value->kind != LHAT_NODE_FUNC ||
            !chk_node_name(c, entry->v.entry.key, &seeded, &seeded_length)) {
            continue;
        }
        LhatType *seed = entry->v.entry.type != NULL
                             ? chk_resolve_type(c, entry->v.entry.type)
                             : chk_declared_signature(c, entry->v.entry.value);
        if (seed != NULL) {
            lhat_type_add_member(c->result->types, table, seeded,
                                 seeded_length, seed);
        }
    }

    // [syntax.obj] makes a table a sequence as well as a mapping. The keyed
    // half is described by name; the sequence half by position, counted the
    // way the machine lays it out -- from zero, in the order written.
    size_t position = 0;
    // [syntax.sub.table-write]: how much of what the entries hold was made here as well.
    uint8_t made = LHAT_OWN_DEEP;
    for (const LhatNode *entry = items; entry != NULL;
         entry = entry->next) {
        // The receiver scope, exactly as chk_infer_def pushes it: self^ is
        // the literal's own type, which is still growing but is the one
        // object -- by the time anything calls the method, it is whole.
        Scope *outer = c->scope;
        Scope receiver;
        bool method = declares_self(c, entry->v.entry.value);
        if (method) {
            chk_scope_open(&receiver, outer, NULL, false);
            Binding *bound =
                chk_scope_add(&receiver, "self^", 5, table, node->offset);
            if (bound != NULL) {
                bound->reached = true;
                bound->declaration = entry->v.entry.value;
            }
            c->scope = &receiver;
        }
        LhatType *annotation = entry->v.entry.type != NULL
                                   ? chk_resolve_type(c, entry->v.entry.type) : NULL;
        LhatType *outer_expected = c->expected_func;
        c->expected_func = annotation;
        LhatType *value = chk_require_value(c, entry->v.entry.value,
                                            chk_infer(c, entry->v.entry.value));
        c->expected_func = outer_expected;
        uint8_t brought = chk_made_depth(c, entry->v.entry.value, value);
        made = brought < made ? brought : made;
        if (annotation != NULL) {
            chk_expect(c, entry->v.entry.value, value, annotation, LHAT_CHECK_ERR_MISMATCH);
            value = annotation;
        }
#if LHAT_WITH_RESOLUTIONS
        ((LhatNode *)entry)->display_type = value;
#endif
        if (method) {
            c->scope = outer;
            chk_scope_close(c, &receiver);
        }

        // [modules.hostvalue]: a table lives on the heap and a host value does not
        // leave the stack, so no member of one is ever a host value.
        if (chk_is_hostvalue(value)) {
            chk_report(c, entry->v.entry.value, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
        // [syntax.type.tuple]: nor a tuple, for the same reason said of slots -- a member
        // is one value, and a run is several. pack^ is what puts one in a
        // table, and it makes the table itself.
        if (lhat_type_tuple_width(value) > 0) {
            chk_report(c, entry->v.entry.value, LHAT_CHECK_ERR_TUPLE_MISPLACED);
        }

        // [syntax.obj.keys-equals]: a computed key is an expression, checked like any other.
        // What it names is only known when it is written out -- an integer
        // reaches the sequence half, a string the keyed one, and anything
        // else lands somewhere [syntax.obj.structure] lets the type stay quiet about.
        if (entry->v.entry.computed) {
            const LhatNode *key = entry->v.entry.key;
            LhatType *asked = chk_require_value(c, key, chk_infer(c, key));
            uint8_t keyed = chk_made_depth(c, key, asked);
            made = keyed < made ? keyed : made;
            // [errors.runtime.index]: nil^ is how "not there" is spelled, so it cannot
            // also be a key. The machine reports one that turns out to be
            // nil^; a key that can only ever be one is decided here. A NaN
            // is the other refusal, and stays the machine's -- nothing in
            // [syntax.obj.number]'s one number type tells them apart.
            if (asked != NULL && asked->kind != LHAT_TYPE_UNKNOWN &&
                asked->kind != LHAT_TYPE_PENDING &&
                lhat_type_conforms(asked, chk_simple(c, LHAT_TYPE_NIL))) {
                chk_report(c, key, LHAT_CHECK_ERR_BAD_KEY);
            }
            // [modules.hostvalue.union]: a literal's key is stored, so only a sealed box
            // holds its hash still -- and a bare host value does not fit a
            // key's one slot at all.
            if (asked != NULL && asked->kind == LHAT_TYPE_HOSTVALUE_BOX &&
                !asked->v.table.sealed) {
                chk_report(c, key, LHAT_CHECK_ERR_MUTABLE_KEY);
            }
            if (chk_is_hostvalue(asked)) {
                chk_report(c, key, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            if (value != NULL && value->kind == LHAT_TYPE_NIL) {
                continue;
            }
            table->v.table.inferred_index = true;
            table->v.table.index_key = lhat_type_union(
                c->result->types, table->v.table.index_key, asked);
            table->v.table.index_value = lhat_type_union(
                c->result->types, table->v.table.index_value, value);
            if (key != NULL && key->kind == LHAT_NODE_INT &&
                key->v.integer.value >= 0) {
                lhat_type_add_index_member(c->result->types, table,
                                           (size_t)key->v.integer.value, value);
            } else if (chk_string_member_key(c, key)) {
                lhat_type_add_member(c->result->types, table,
                                     c->lexer->strings + key->v.string.offset,
                                     key->v.string.length, value);
            }
            continue;
        }

        const char *name = NULL;
        size_t length = 0;
        chk_refuse_hat_name(c, entry->v.entry.key, true);
        if (chk_node_name(c, entry->v.entry.key, &name, &length)) {
            chk_check_tostring_shape(c, entry, value, name, length, false);
            // A seeded signature is written over with what the body turned
            // out to be -- one member, never two of one name.
            const LhatTypeMember *already =
                chk_find_member(table, name, length);
            if (already != NULL) {
                ((LhatTypeMember *)already)->type = value;
            } else {
                lhat_type_add_member(c->result->types, table, name, length,
                                     value);
            }
            continue;
        }
        if (entry->v.entry.key == NULL) {
            lhat_type_add_index_member(c->result->types, table, position++,
                                       value);
        }
    }
    ((LhatNode *)node)->checked_own =
        made == LHAT_OWN_DEEP ? made : (uint8_t)(made + 1);
    if (table->v.table.index_key != NULL) {
        chk_join_table_entry_types(c, table, &table->v.table.index_key,
                                &table->v.table.index_value);
    }
    return table;
}

// [syntax.obj]. A definition produces two structures, and [syntax.obj.instance] is what ties them:
// an instance can reach the definition's members, so its type contains them
// as well as the fields the template declares.
//
//   definition : the members, plus a new if none was written ([syntax.obj.new])
//   instance   : those members, plus the template's fields
//
// [syntax.obj.name] keeps the name out of it. Both are ordinary structures, so [syntax.op.semantic]'s
// structural identity applies unchanged and nothing here has to be interned.
// The one walk of a member list, under both of the searches below.
const LhatTypeMember *chk_members_search(const LhatTypeMember *members,
                                         const char *name, size_t length)
{
    for (; members != NULL; members = members->next) {
        if (members->name_length == length &&
            memcmp(members->name, name, length) == 0) {
            return members;
        }
    }
    return NULL;
}

const LhatTypeMember *chk_find_member(const LhatType *table,
                                      const char *name, size_t length)
{
    // [modules.hostvalue]: a host value type keeps its registered members in the same
    // list a table keeps its own, so the one search serves both -- and the
    // links it may hang off are type.c's to follow, since the machine and
    // the relations read them through the same walk.
    return lhat_type_find_member(table, name, length);
}

// [syntax.obj.override]'s overload^ puts several signatures under one name, and [syntax.obj.new]'s new
// is a name like any other -- so what a definition holds there is one
// signature or an intersection of them. Both readings below take that as
// given: every arm answers with the same instance, since every arm is a way
// of constructing this definition.
static const LhatType *constructor_arm(const LhatType *held)
{
    if (held == NULL) {
        return NULL;
    }
    if (held->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = held->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type != NULL && arm->type->kind == LHAT_TYPE_FUNC) {
                return arm->type;
            }
        }
        return NULL;
    }
    return held->kind == LHAT_TYPE_FUNC ? held : NULL;
}

// [syntax.obj.new] makes new return an instance, so a definition's own structure is
// where its instance type can be found again. Composition needs it, to carry
// the base's fields into the derived instance.
LhatType *chk_instance_of(const LhatType *definition)
{
    // [modules.hostdata]: a registered host type is its own written type -- it has
    // no instance half, and a member the host happened to call "new"
    // (std.math.Vector3.new, say) is an ordinary constructor function, not
    // [syntax.obj.new]'s mechanism. Without this, writing the type's name answered
    // with that member's result instead of the type.
    if (definition != NULL && definition->kind == LHAT_TYPE_TABLE &&
        definition->v.table.nominal) {
        return NULL;
    }
    // [syntax.obj.members-order]: a def^ says outright what its instances carry. Read before new
    // is asked, since the two must not drift -- what new answers is built
    // from this very structure.
    if (definition != NULL && definition->kind == LHAT_TYPE_TABLE &&
        definition->v.table.instance != NULL) {
        return definition->v.table.instance;
    }
    // [syntax.obj.new] is a def^'s mechanism and no one else's. A module -- hosted, or
    // what require^ answers -- may carry an ordinary function under the
    // name (std.regex.new, say), and reading its result as "the instance
    // type" here made the module's own name unresolvable as a path.
    if (definition == NULL || definition->kind != LHAT_TYPE_TABLE ||
        !definition->v.table.is_definition) {
        return NULL;
    }
    const LhatTypeMember *constructor = chk_find_member(definition, "new", 3);
    const LhatType *arm =
        constructor != NULL ? constructor_arm(constructor->type) : NULL;
    return arm != NULL ? arm->v.func.result : NULL;
}

// [syntax.obj.new] with [syntax.obj.compose]: the new a definition gets before anything is written into
// it. The parameters come from the base -- a derived definition is built the
// way the base was -- but the result has to be the derived instance, since
// [syntax.obj.compose] composes to make something new and a constructor answering with the
// base would defeat that. With no base, or a base that carries no signature
// here, it is the one [syntax.obj.new] gives every definition: no arguments, and the
// template fixes every field.
//
// An overloaded new is rebuilt arm by arm, since each arm is one way of
// constructing the same thing.
static LhatType *constructor_from(Checker *c, const LhatType *inherited,
                                  LhatType *instance)
{
    if (inherited != NULL && inherited->kind == LHAT_TYPE_INTERSECT) {
        LhatType *rebuilt = NULL;
        for (const LhatTypeList *arm = inherited->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type == NULL || arm->type->kind != LHAT_TYPE_FUNC) {
                continue;
            }
            LhatType *one = constructor_from(c, arm->type, instance);
            rebuilt = rebuilt == NULL
                          ? one
                          : lhat_type_intersect(c->result->types, rebuilt, one);
        }
        if (rebuilt != NULL) {
            return rebuilt;
        }
    }

    LhatType *constructor = lhat_type_func(c->result->types, true);
    if (inherited != NULL && inherited->kind == LHAT_TYPE_FUNC) {
        for (const LhatTypeList *p = inherited->v.func.params; p != NULL;
             p = p->next) {
            lhat_type_add_param(c->result->types, constructor, p->type);
        }
        constructor->v.func.variadic = inherited->v.func.variadic;
    }
    constructor->v.func.result = instance;
    return constructor;
}

// The same, reading the base's new off the base itself.
static LhatType *constructor_for(Checker *c, const LhatType *base,
                                 LhatType *instance)
{
    const LhatTypeMember *inherited = chk_find_member(base, "new", 3);
    return constructor_from(c, inherited != NULL ? inherited->type : NULL,
                            instance);
}

// [syntax.obj.new]: the leaves a template initialiser may leave on the prototype
// by type alone. Construction copies the prototype, so a value anything
// could write through -- a coroutine, a host object -- would be one place
// shared by all of them. Immutable values pass: a subroutine's captured
// places are its own affair, the way a definition's shared members already
// are, and an error kind is an identity. So does a definition -- a public
// identity every instance already shares by name, which a field adds no
// sharing to. any^ could smuggle a mutable one past this, so it is refused;
// what inference left open is let through, since the machine checks the
// values themselves (SETPROTO). A plain table is not a leaf -- what admits
// one is the literal-tree reading below.
static bool immutable_default_type(const LhatType *type)
{
    if (type == NULL) {
        return true;
    }
    switch (type->kind) {
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
        case LHAT_TYPE_NIL:
        case LHAT_TYPE_BOOL:
        case LHAT_TYPE_NUMBER:
        case LHAT_TYPE_STRING:
        case LHAT_TYPE_FUNC:
        case LHAT_TYPE_ERROR_KIND:
            return true;
        case LHAT_TYPE_TABLE:
            return type->v.table.is_definition;
        // [modules.hostvalue]: a box is a copyable node -- bake and construction copy
        // its bytes, so a default shares nothing.
        case LHAT_TYPE_HOSTVALUE_BOX:
            return true;
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (!immutable_default_type(arm->type)) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

// [syntax.obj.new]: whether this initialiser may sit on the prototype. A table
// only as a literal tree -- table literals nested in table literals, each
// level born fresh at this very expression, so the copies construction hands
// out share nothing and cycle nowhere, and the whole of what every instance
// starts with is right there in the source. A table that arrived any other
// way -- a name, a call's answer -- is an identity from somewhere else,
// which is what new is for. The entry types are read off the literal's own
// inferred type, walked in parallel with the tree.
static bool immutable_default_value(Checker *c, const LhatNode *node,
                                    const LhatType *type)
{
    if (node != NULL && (node->kind == LHAT_NODE_TABLE ||
        (node->kind == LHAT_NODE_NEW && type != NULL &&
         type->kind == LHAT_TYPE_TABLE && !type->v.table.from_definition))) {
        const LhatNode *items = node->kind == LHAT_NODE_NEW
                                    ? node->v.named.members : node->v.list.items;
        size_t position = 0;
        for (const LhatNode *entry = items; entry != NULL;
             entry = entry->next) {
            // A computed key is a value of its own; the names and numbers a
            // literal writes out are the keys a default takes.
            if (entry->v.entry.computed) {
                return false;
            }
            const LhatTypeMember *member = NULL;
            const char *name = NULL;
            size_t length = 0;
            if (entry->v.entry.key != NULL) {
                if (chk_node_name(c, entry->v.entry.key, &name, &length)) {
                    member = chk_find_member(type, name, length);
                }
            } else {
                member = lhat_type_member_at(type, position++);
            }
            if (!immutable_default_value(c, entry->v.entry.value,
                                         member != NULL ? member->type
                                                        : NULL)) {
                return false;
            }
        }
        return true;
    }
    return immutable_default_type(type);
}

// [syntax.obj.super] with [syntax.obj.new]: what super^ names inside a written new -- the
// hook of the new standing before it, run against the same receiver. It
// takes what that new took and answers nothing: the instance is already in
// the caller's hands, so there is nothing for the chain to pass back.
static LhatType *hook_signature(Checker *c, const LhatType *from)
{
    if (from != NULL && from->kind == LHAT_TYPE_INTERSECT) {
        LhatType *rebuilt = NULL;
        for (const LhatTypeList *arm = from->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (arm->type == NULL || arm->type->kind != LHAT_TYPE_FUNC) {
                continue;
            }
            LhatType *one = hook_signature(c, arm->type);
            rebuilt = rebuilt == NULL
                          ? one
                          : lhat_type_intersect(c->result->types, rebuilt, one);
        }
        if (rebuilt != NULL) {
            return rebuilt;
        }
    }
    LhatType *hook = lhat_type_func(c->result->types, true);
    if (from != NULL && from->kind == LHAT_TYPE_FUNC) {
        for (const LhatTypeList *p = from->v.func.params; p != NULL;
             p = p->next) {
            lhat_type_add_param(c->result->types, hook, p->type);
        }
        hook->v.func.variadic = from->v.func.variadic;
    }
    return hook;
}

// Overwrites rather than appending, so a member the base already had ends up
// replaced by [syntax.obj.override]'s override^ instead of shadowed by position.
//
// [syntax.obj.abstract]: `abstract` travels with the type, so writing over a declaration with
// a real one is what fills the hole -- and writing a declaration over a
// declaration leaves it open.
// Answers the member it settled -- the one already there, or the one it
// added -- so that a caller holding the key it was written under can say
// where that key stands ([lsp.hover], chk_member_declared_at).
static LhatTypeMember *set_member_marked(Checker *c, LhatType *table,
                                         const char *name, size_t length,
                                         LhatType *type, bool abstract,
                                         bool pending)
{
    // The table's own, and no link followed: [modules.hostdata.base]'s base and [syntax.obj.delegate]'s
    // delegate lend a name without giving it, and writing through either
    // would put a member on somebody else's type.
    LhatTypeMember *m =
        (LhatTypeMember *)lhat_type_own_member(table, name, length);
    if (m != NULL) {
        m->type = type;
        m->abstract = abstract;
        m->pending = pending;
        // [syntax.obj.members-order]: whatever the first pass put here, this is the real one.
        m->provisional = false;
        return m;
    }
    LhatTypeMember *added = lhat_type_add_member(c->result->types, table, name,
                                                 length, type);
    if (added != NULL) {
        added->abstract = abstract;
        added->pending = pending;
    }
    return added;
}

// [syntax.obj.members-order]: the first pass's seed. Puts the name there with the signature
// it was written with, so a body checked before it can reach it -- and leaves
// alone anything already there, which is the base's or an earlier entry's and
// is the real thing rather than a seed.
static LhatTypeMember *set_member_provisional(Checker *c, LhatType *table,
                                              const char *name, size_t length,
                                              LhatType *type, bool abstract)
{
    LhatTypeMember *m =
        (LhatTypeMember *)lhat_type_own_member(table, name, length);
    if (m != NULL) {
        return m;
    }
    LhatTypeMember *added = lhat_type_add_member(c->result->types, table, name,
                                                 length, type);
    if (added != NULL) {
        added->provisional = true;
        added->abstract = abstract;
    }
    return added;
}

// [syntax.obj.delegate]: the next walk's seed. What the last walk inferred stays --
// that is what it learned -- and the mark says it is still not the answer, so
// a body reading it is known to have read ahead again, and [syntax.obj.override]'s "a second
// member of this name wants a marker" does not fire on this def^'s own
// entries the way it would against a member that is really there.
static void mark_provisional(LhatType *table, const char *name, size_t length)
{
    LhatTypeMember *m =
        (LhatTypeMember *)lhat_type_own_member(table, name, length);
    if (m != NULL) {
        m->provisional = true;
    }
}

static LhatTypeMember *set_member_as(Checker *c, LhatType *table,
                                     const char *name, size_t length,
                                     LhatType *type, bool abstract)
{
    return set_member_marked(c, table, name, length, type, abstract, false);
}

static LhatTypeMember *set_member(Checker *c, LhatType *table,
                                  const char *name, size_t length,
                                  LhatType *type)
{
    return set_member_marked(c, table, name, length, type, false, false);
}

// [syntax.obj.abstract]: whether the definition is still waiting on a composition. [syntax.obj.new]'s
// new is what this stands in the way of -- an abstract^ would leave a name
// with nothing under it, and [syntax.obj.abstract-override]'s pending override^ would leave a super^
// pointing at nothing.
//
// `field` answers which of the two lists the hole is in, since [syntax.obj.abstract-new] gives
// the two different ways out: a member is a composition's to provide, and a
// field is that or a written new's to write.
const LhatTypeMember *chk_hole_of(const LhatType *definition, bool *field)
{
    if (field != NULL) {
        *field = false;
    }
    if (definition == NULL || definition->kind != LHAT_TYPE_TABLE) {
        return NULL;
    }
    for (const LhatTypeMember *m = definition->v.table.members; m != NULL;
         m = m->next) {
        if (m->abstract || m->pending) {
            return m;
        }
    }
    const LhatType *instance = chk_instance_of(definition);
    if (instance == NULL || instance->kind != LHAT_TYPE_TABLE) {
        return NULL;
    }
    for (const LhatTypeMember *m = instance->v.table.members; m != NULL;
         m = m->next) {
        if (m->abstract || m->pending) {
            if (field != NULL) {
                *field = !m->pending;  // a wait is [syntax.obj.abstract-override]'s, not a field's
            }
            return m;
        }
    }
    return NULL;
}

const LhatTypeMember *chk_unimplemented_member(const LhatType *definition)
{
    return chk_hole_of(definition, NULL);
}

// Composition copies availability as well as the signature. In particular,
// extending a definition cannot make an ambiguous name callable again.
static LhatTypeMember *copy_member(Checker *c, LhatType *into,
                                   const LhatTypeMember *from)
{
    LhatTypeMember *copy = set_member_marked(
        c, into, from->name, from->name_length, from->type,
        from->abstract, from->pending);
    if (copy != NULL) {
        copy->ambiguous = from->ambiguous;
        copy->demanded = from->demanded;
        copy->instance_field = from->instance_field;
    }
    return copy;
}

static void copy_members(Checker *c, LhatType *into, const LhatType *from)
{
    if (from == NULL || from->kind != LHAT_TYPE_TABLE) {
        return;
    }
    for (const LhatTypeMember *m = from->v.table.members; m != NULL;
         m = m->next) {
        copy_member(c, into, m);
    }
}

// [syntax.obj.delegate]: does this member take a receiver? That is [syntax.obj.instance]'s whole rule for
// what an instance sees, and delegation does not widen it -- a static member
// of the delegate is the delegate's, the way a static member of a definition
// is the definition's.
//
// [syntax.obj.override]'s groups answer yes when any arm does: the call picks an arm, and one
// that takes a receiver is reachable through the dot.
static bool member_takes_receiver(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_FUNC) {
        return type->v.func.takes_self;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (member_takes_receiver(arm->type)) {
                return true;
            }
        }
    }
    return false;
}

// What this def^ delegates to, as it was written. NULL when it declares
// none. [syntax.obj.methods]'s two spellings: 'self^.x' reads the receiver's field, a bare
// 'x' the definition's member, and `through_self` is which was written --
// they are not one form with an optional prefix, since [syntax.obj.methods] declares fields
// and members apart and a name may be both. The same split decides where
// the answer is looked for at run time.
static const LhatNode *delegate_of(const LhatNode *node, bool *through_self)
{
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.modifier != LHAT_DEF_DELEGATE ||
            entry->v.entry.value == NULL) {
            continue;
        }
        if (through_self != NULL) {
            *through_self = entry->v.entry.value->kind == LHAT_NODE_MEMBER;
        }
        return entry->v.entry.value;  // the parser allows one
    }
    return NULL;
}

// [syntax.obj.delegate]: hangs what this definition delegates to off the section its
// instances carry, as a link. Nothing is copied: chk_find_member follows it
// where a name is not the wrapper's own, which is [syntax.obj.instance]'s order, and the
// machine answers the same name by growing a leg on the same walk
// (object.c). One shape, asked twice.
//
// Answers whether it settled. A delegate naming a member whose type only its
// own body knows cannot settle before that body is walked, so this is asked
// once per round of the walk ([pipeline.infer.rounds]) and reports only on the last --
// `report` is what tells the two apart, and a walk that did not settle says
// so through read_provisional, which is what buys the next round.
static bool link_delegate(Checker *c, const LhatNode *node,
                          LhatType *definition, LhatType *instance,
                          bool report)
{
    bool through_self = false;
    const LhatNode *named = delegate_of(node, &through_self);
    if (named == NULL || instance == NULL) {
        return true;  // nothing to settle
    }

    const LhatNode *spelt = through_self ? named->v.access.argument : named;
    const char *name = NULL;
    size_t length = 0;
    if (!chk_node_name(c, spelt, &name, &length)) {
        return true;  // not a name; the walk of it says what is wrong
    }
    // Looked for where the spelling said, and nowhere else. Finding it in the
    // other place would be answering a question that was not asked.
    const LhatTypeMember *held =
        lhat_type_own_member(through_self ? instance : definition, name, length);
    LhatType *from = held != NULL ? held->type : NULL;
    if (from == NULL || from->kind != LHAT_TYPE_TABLE) {
        // [syntax.obj.def-form] gives a member no way to carry a written type, so a static
        // one holding what a call answered is known only once that entry has
        // been walked. Saying it was read ahead is what asks for the round
        // that walks it; on the last round `report` is set and this is the
        // answer.
        if (!report) {
            c->read_provisional = true;
            return false;
        }
        if (held == NULL) {
            chk_report_named(c, spelt, LHAT_CHECK_ERR_NO_MEMBER, name, length);
        } else {
            chk_report(c, spelt, LHAT_CHECK_ERR_NO_MEMBER);
        }
        return true;
    }
    // [syntax.obj.instance]: an instance sees what the delegate's own instances would see. A
    // host type ([modules.hostdata]) has no instance section of its own -- its members
    // ARE what one reaches -- so the type itself is what is linked, and its
    // base link carries the rest of the chain.
#if LHAT_WITH_RESOLUTIONS
    // [lsp.hover]: what the spelling named is a member, found here and nowhere
    // else -- [syntax.obj.delegate]'s target is not walked as an expression, so nothing
    // else records it and a reader standing on it was told nothing. What was
    // found is the member itself, so where it was written comes with it.
    chk_record_member_resolution(c, spelt, from, held, false);
#endif
    instance->v.table.delegate =
        from->v.table.instance != NULL ? from->v.table.instance : from;
    return true;
}

// [syntax.obj.abstract-new]: which fields a written new gives a value to.
//
// [syntax.obj.new] builds an instance with `self^{ … }`, and [syntax.obj.abstract]'s declaration on a
// template field is a field with no initialiser -- so a new that writes one is
// what an instance of that definition gets it from. Reading which names it
// writes is what tells the hole from the filled field, and the tree is where
// that is: the walk over the fields (the SELF_TABLE case) asks of each written
// field whether the template has it, which is the other direction.
//
// Every self^{ … } in the body has to write the name, since any of them may be
// the one that runs -- so several are intersected rather than unioned.
typedef struct {
    Checker *c;
    const char *names[LHAT_CHECK_MAX_TRACKED_ARGS];
    size_t lengths[LHAT_CHECK_MAX_TRACKED_ARGS];
    size_t count;
    bool seen_one;   // whether a first self^{ … } has been met to intersect with
    bool overflowed; // more written fields than there is room to track
} WrittenFields;

static void collect_written_fields(WrittenFields *w, const LhatNode *node);

static void written_fields_child(void *context, const char *field, bool in_list,
                                 const LhatNode *child)
{
    (void)field;
    (void)in_list;
    collect_written_fields((WrittenFields *)context, child);
}

// Keeps only the names this one also writes, which is the intersection two
// branches of a new agree on.
static void intersect_written(WrittenFields *w, const LhatNode *table)
{
    size_t kept = 0;
    for (size_t i = 0; i < w->count; i++) {
        bool here = false;
        for (const LhatNode *field = table->v.list.items;
             field != NULL && !here; field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            if (chk_node_name(w->c, field->v.entry.key, &name, &length)) {
                here = length == w->lengths[i] &&
                       memcmp(name, w->names[i], length) == 0;
            }
        }
        if (here) {
            w->names[kept] = w->names[i];
            w->lengths[kept] = w->lengths[i];
            kept++;
        }
    }
    w->count = kept;
}

static void collect_written_fields(WrittenFields *w, const LhatNode *node)
{
    if (node == NULL) {
        return;
    }
    // A body written inside this one builds some other definition's instance,
    // and its self^{ … } says nothing about this one's fields.
    if (node->kind == LHAT_NODE_FUNC || node->kind == LHAT_NODE_DEF) {
        return;
    }
    if (node->kind == LHAT_NODE_SELF_TABLE) {
        if (w->seen_one) {
            intersect_written(w, node);
            return;
        }
        w->seen_one = true;
        for (const LhatNode *field = node->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            if (!chk_node_name(w->c, field->v.entry.key, &name, &length)) {
                continue;
            }
            if (w->count == LHAT_CHECK_MAX_TRACKED_ARGS) {
                w->overflowed = true;
                break;
            }
            w->names[w->count] = name;
            w->lengths[w->count] = length;
            w->count++;
        }
        return;
    }
    lhat_node_visit_children(node, written_fields_child, w);
}

// [lsp.next]: [syntax.obj.override]'s marker leads the member it applies to, and the parser starts
// the entry at it -- so the marker is the stretch between the entry's start
// and its key, the space after the word included. Empty length when no marker
// was written, which is how a fix knows there is nothing to take off.
static size_t marker_removal(const LhatNode *entry, LhatFixSlot *out)
{
    const LhatNode *key = entry->v.entry.key;
    if (key == NULL || key->offset <= entry->offset) {
        return 0;
    }
    *out = lhat_fix_slot(LHAT_FIX_REMOVE_MARKER, LHAT_FIX_SUGGESTED,
                         entry->offset, key->offset - entry->offset, "");
    return 1;
}

// And the other way: a name that is already a member takes one of the two
// markers, written in front of it. Which of them is the writer's to say --
// one replaces what is there, the other adds a way of calling it -- so both
// are offered and neither is machine-appliable. Fills LHAT_FIX_SLOTS slots.
static size_t marker_choices(const LhatNode *entry, LhatFixSlot *out)
{
    static const struct {
        LhatFixTitle title;
        const char *text;
    } WORDS[] = {
        {LHAT_FIX_WRITE_OVERRIDE, "override^ "},
        {LHAT_FIX_WRITE_OVERLOAD, "overload^ "},
    };
    for (size_t i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
        out[i] = lhat_fix_slot(WORDS[i].title, LHAT_FIX_SUGGESTED,
                               entry->offset, 0, WORDS[i].text);
    }
    return sizeof WORDS / sizeof WORDS[0];
}

// [syntax.obj.override]'s overlap test, applied to two signatures: is there an argument count
// admissible by both at which no position is separate? A separate position is
// enough to keep them apart, since a call has to satisfy every one.
//
// [syntax.type.param-names] keeps defaults out of a type, so a signature admits one count, or a
// range once it has a '...'. Checking the smallest count they share settles
// it: beyond that at least one side is variadic, and its element type does
// not change with position.
static bool signatures_overlap(const LhatType *a, const LhatType *b)
{
    if (a == NULL || b == NULL || a->kind != LHAT_TYPE_FUNC ||
        b->kind != LHAT_TYPE_FUNC) {
        return true;  // nothing here can tell them apart
    }

    // [syntax.op.right]: which side the receiver stands on is a position too, and the
    // one a call fixes first. An operator written 'f^self^, rhs' answers a
    // use with its owner on the left and 'f^lhs, self^' one with it on the
    // right; [syntax.op.right]'s order asks the left operand first and only then the
    // right, so no use ever reaches both. The two are different types
    // (type.h) and chk_operator_arm already picks between them by this alone.
    if (a->v.func.self_last != b->v.func.self_last) {
        return false;
    }

    size_t count_a = 0;
    size_t count_b = 0;
    for (const LhatTypeList *p = a->v.func.params; p != NULL; p = p->next) {
        count_a++;
    }
    for (const LhatTypeList *p = b->v.func.params; p != NULL; p = p->next) {
        count_b++;
    }

    size_t shared = count_a > count_b ? count_a : count_b;
    if ((shared > count_a && a->v.func.variadic == NULL) ||
        (shared > count_b && b->v.func.variadic == NULL)) {
        return false;
    }

    const LhatTypeList *pa = a->v.func.params;
    const LhatTypeList *pb = b->v.func.params;
    for (size_t i = 0; i < shared; i++) {
        LhatType *ta = pa != NULL ? pa->type : a->v.func.variadic;
        LhatType *tb = pb != NULL ? pb->type : b->v.func.variadic;
        if (lhat_type_disjoint(ta, tb)) {
            return false;
        }
        if (pa != NULL) {
            pa = pa->next;
        }
        if (pb != NULL) {
            pb = pb->next;
        }
    }
    return true;
}

// [syntax.obj.override]. A member of a name the base already uses is an error unless it says
// which of the two things it means, and each says something checkable.
// [syntax.obj.override]: an override^ replaces the one candidate its signature overlaps.
// What a name carries is a single signature or an intersection of them
// ([syntax.obj.super]), so this walks the arms, checks substitutability against the one
// that overlaps, and answers the intersection with that arm swapped out.
//
// Refuses two overlaps -- [syntax.obj.override] says which one was meant is then undecidable
// -- and none, since there was nothing at that name to replace.
static LhatType *override_one(Checker *c, const LhatNode *entry,
                              LhatType *inherited, LhatType *replacement)
{
    if (inherited == NULL || inherited->kind != LHAT_TYPE_INTERSECT) {
        // One candidate, so it is the one: ordinary conformance, arguments
        // wider and result narrower. The receiver is not in the parameter
        // list ([syntax.obj.self]), so [syntax.obj.override]'s exemption of self^ needs nothing of its own.
        if (!lhat_type_conforms(replacement, inherited)) {
            chk_report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
        }
        return replacement;
    }

    LhatType *overlapped = NULL;
    size_t overlaps = 0;
    size_t position = 0;
    size_t at = 0;
    for (const LhatTypeList *arm = inherited->v.composite.arms; arm != NULL;
         arm = arm->next, position++) {
        if (signatures_overlap(replacement, arm->type)) {
            overlapped = arm->type;
            at = position;
            overlaps++;
        }
    }

    if (overlaps == 0) {
        LhatFixSlot away;
        chk_report_fix(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, NULL, 0,
                       &away, marker_removal(entry, &away));
        return replacement;
    }
    if (overlaps > 1) {
        // [syntax.obj.override]: widening the arguments may reach two of them, and then
        // which is being replaced is not decidable. Reported as the overlap
        // it is, since that is what the writer has to take apart.
        chk_report(c, entry, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
        return replacement;
    }
    if (!lhat_type_conforms(replacement, overlapped)) {
        chk_report(c, entry, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE);
        return replacement;
    }

    // [spec.arms]: which arm this is settles the shape of the group the run
    // builds, so the compiler is told rather than left to put the replacement
    // in front of a group that keeps the arm it replaced. Not a strict-only
    // matter: [pipeline.repl.no-fork] asks that what runs be the same either way, and the
    // arms a name carries are read by typeof^ ([syntax.obj.typeof]) whichever it is.
    if (at + 1 <= UINT16_MAX) {
        ((LhatNode *)entry)->checked_arm = (uint16_t)(at + 1);
    }

    // The others are untouched, so the name goes on carrying them.
    LhatType *rebuilt = NULL;
    for (const LhatTypeList *arm = inherited->v.composite.arms; arm != NULL;
         arm = arm->next) {
        LhatType *part = arm->type == overlapped ? replacement : arm->type;
        rebuilt = rebuilt == NULL
                      ? part
                      : lhat_type_intersect(c->result->types, rebuilt, part);
    }
    return rebuilt != NULL ? rebuilt : replacement;
}

// `constructor` says this entry is [syntax.obj.new]'s new, which [syntax.obj.override-new] exempts from
// the substitutability an override^ otherwise owes. Nothing can be written
// that takes a definition where another definition is expected -- [modules.env.one]
// and [syntax.obj.instance] make a definition's name mean its instance -- so a new that
// narrows what it accepts leaves no caller behind. A method is the other way
// round: an instance does stand where the base's instance is written, and
// there the rule is holding something up. So an override^ new replaces what
// the name held, whole.
static LhatType *check_same_name(Checker *c, const LhatNode *entry,
                                 const LhatTypeMember *inherited,
                                 LhatType *replacement, bool constructor)
{
    LhatDefModifier modifier = entry->v.entry.modifier;

    if (inherited == NULL) {
        // [syntax.obj.abstract-override]: an override^ with nothing yet under the name is a mixin
        // written against a base it has not met. It is not an error -- it
        // says what the composition has to bring, and the member stays
        // pending until something does. [syntax.obj.new]'s new is what it stands in
        // the way of; overload^ has no such reading, since adding a way to
        // call something that is not there says nothing.
        if (modifier == LHAT_DEF_OVERLOAD) {
            LhatFixSlot away;
            chk_report_fix(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, NULL,
                           0, &away, marker_removal(entry, &away));
        }
        return replacement;
    }

    if (inherited->ambiguous) {
        chk_report(c, entry, LHAT_CHECK_ERR_AMBIGUOUS_MEMBER);
        return replacement;
    }

    // [syntax.obj.abstract]: a declaration is not a definition of the member, so filling it
    // in is not the collision [syntax.obj.override] is about -- no marker is wanted. What the
    // declaration does ask is that the value fit the type it wrote.
    if (inherited->abstract) {
        if (modifier != LHAT_DEF_PLAIN) {
            LhatFixSlot away;
            chk_report_fix(c, entry, LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE, NULL,
                           0, &away, marker_removal(entry, &away));
        } else if (!lhat_type_conforms(replacement, inherited->type)) {
            chk_report(c, entry, LHAT_CHECK_ERR_MISMATCH);
        }
        return replacement;
    }

    switch (modifier) {
        case LHAT_DEF_PLAIN: {
            LhatFixSlot markers[LHAT_FIX_SLOTS];
            chk_report_fix(c, entry, LHAT_CHECK_ERR_MEMBER_EXISTS, NULL, 0,
                           markers, marker_choices(entry, markers));
            return replacement;
        }

        case LHAT_DEF_OVERRIDE:
            // [syntax.obj.override-new]: the exemption above. The whole member goes, so there
            // is no arm to name and nothing to compare against.
            if (constructor) {
                return replacement;
            }
            // [syntax.obj.override]: what is replaced is the one existing candidate the new
            // signature overlaps. A name that was overloaded carries several,
            // and comparing against all of them at once would refuse every
            // replacement -- no one signature is usable where an intersection
            // of them was.
            return override_one(c, entry, inherited->type, replacement);

        case LHAT_DEF_OVERLOAD: {
            // [syntax.obj.override]: the ban is on one signature overlapping another, so a
            // name already overloaded is asked arm by arm -- the same reason
            // override^ above walks them. An intersection is not a signature,
            // and signatures_overlap answers "cannot tell them apart" for
            // anything that is not one, so asking it as a whole would refuse
            // every arm past the second.
            bool overlaps = false;
            if (inherited->type != NULL &&
                inherited->type->kind == LHAT_TYPE_INTERSECT) {
                for (const LhatTypeList *arm = inherited->type->v.composite.arms;
                     arm != NULL && !overlaps; arm = arm->next) {
                    overlaps = signatures_overlap(replacement, arm->type);
                }
            } else {
                overlaps = signatures_overlap(replacement, inherited->type);
            }
            if (overlaps) {
                chk_report(c, entry, LHAT_CHECK_ERR_OVERLOAD_OVERLAPS);
                return replacement;
            }
            // [syntax.obj.override]: an overloaded member is callable both ways, which is
            // what '&' means ([syntax.obj.compose]).
            return lhat_type_intersect(c->result->types, inherited->type,
                                       replacement);
        }

        // [syntax.obj.abstract]'s declaration and [syntax.obj.delegate]'s delegate^ define no member, so
        // [syntax.obj.override] has nothing to ask of either.
        case LHAT_DEF_ABSTRACT:
        case LHAT_DEF_DELEGATE:
            break;
    }
    return replacement;
}

// Merge one name from an independently written definition. A field has no
// qualified access to fall back to; a shared member does, so only the field
// collision rejects the composition itself.
static void compose_member(Checker *c, const LhatNode *node, LhatType *into,
                           const LhatTypeMember *right, bool field)
{
    LhatTypeMember *left = (LhatTypeMember *)lhat_type_own_member(
        into, right->name, right->name_length);
    if (left == NULL) {
        copy_member(c, into, right);
        return;
    }
    if (left->instance_field != right->instance_field) {
        chk_report_named(c, node, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                         right->name, right->name_length);
        return;
    }
    if (left->ambiguous || right->ambiguous) {
        // Neither an abstract requirement nor a pending override can choose
        // a provider from an ambiguous name. Keep any unmet requirement open.
        if (right->abstract) {
            left->type = left->abstract
                ? lhat_type_intersect(c->result->types, left->type, right->type)
                : right->type;
        }
        // A requirement a side had already met is open again: what met it is
        // one of the two answers that reach no one.
        left->abstract = left->abstract || right->abstract ||
                         left->demanded || right->demanded;
        left->demanded = left->demanded || right->demanded;
        left->pending = left->pending || right->pending;
        left->ambiguous = true;
        return;
    }
    if (left->abstract && right->abstract) {
        // Both requirements survive, independent of the order of the parts.
        left->type = lhat_type_intersect(c->result->types, left->type,
                                        right->type);
        return;
    }
    if (left->abstract || right->abstract) {
        const LhatTypeMember *need = left->abstract ? left : right;
        const LhatTypeMember *give = left->abstract ? right : left;
        if (!lhat_type_conforms(give->type, need->type)) {
            chk_report_named(c, node, LHAT_CHECK_ERR_MISMATCH,
                             right->name, right->name_length);
        }
        LhatTypeMember *met = copy_member(c, into, give);
        if (met != NULL) {
            met->demanded = true;
        }
        return;
    }
    if (right->pending) {
        if (!lhat_type_conforms(right->type, left->type)) {
            chk_report_named(c, node, LHAT_CHECK_ERR_NOT_SUBSTITUTABLE,
                             right->name, right->name_length);
        }
        bool pending = left->pending;
        bool demanded = left->demanded || right->demanded;
        LhatTypeMember *written = copy_member(c, into, right);
        if (written != NULL) {
            written->pending = pending;
            written->demanded = demanded;
        }
    } else if (field) {
        chk_report_named(c, node, LHAT_CHECK_ERR_COMPOSE_COLLIDES,
                         right->name, right->name_length);
    } else {
        // [syntax.obj.compose-ambiguous]: carried by both sides, so it reaches no one answer. Marked
        // rather than dropped, so an access can say what went wrong rather
        // than that the name is not there -- and a requirement either side
        // had met is open again, as above.
        left->ambiguous = true;
        if (left->demanded || right->demanded) {
            left->abstract = true;
            left->demanded = true;
        }
    }
}

// [syntax.obj.compose]: a name on the right brings its type and any pending requirements.
// Unlike a literal, it was not written against this left side. The compiler
// flattens the same parts, so the type must preserve every unresolved name.
LhatType *chk_compose_definitions(Checker *c, const LhatNode *node,
                                  LhatType *left, LhatType *right)
{
    const LhatDefinition *a = node->v.binary.left->checked_definition;
    const LhatDefinition *b = node->v.binary.right->checked_definition;
    if (a != NULL && b != NULL) {
        LhatDefinition *joined = lhat_definition_new(c->result->types, NULL);
        if (joined != NULL) {
            joined->left = a;
            joined->right = b;
        }
        ((LhatNode *)node)->checked_definition = joined;
    }
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    definition->v.table.is_definition = true;
    definition->v.table.instance = instance;  // [syntax.obj.instance]
    definition->v.table.from_definition = true;
    instance->v.table.from_definition = true;

    copy_members(c, definition, left);
    copy_members(c, instance, chk_instance_of(left));
    // [syntax.obj.delegate]: and what the parts delegate to, which is a link and so carries
    // over whole. The last part to declare one wins, the way a member written
    // later does -- which is how the compiler reads a composition (it walks
    // every part of the chain for a delegate^), and how the literal form of
    // '..' below already carried its base's. This form carried none, so a
    // member the machine lends through the composition was one the checker
    // called missing.
    const LhatType *left_instance = chk_instance_of(left);
    const LhatType *lent_right = chk_instance_of(right);
    if (lent_right != NULL && lent_right->kind == LHAT_TYPE_TABLE &&
        lent_right->v.table.delegate != NULL) {
        instance->v.table.delegate = lent_right->v.table.delegate;
    } else if (left_instance != NULL &&
               left_instance->kind == LHAT_TYPE_TABLE) {
        instance->v.table.delegate = left_instance->v.table.delegate;
    }
    for (const LhatTypeMember *m = right->v.table.members; m != NULL;
         m = m->next) {
        // Every definition has new; it is rebuilt for the composed instance.
        if (!chk_name_is(m->name, m->name_length, "new")) {
            compose_member(c, node, definition, m, false);
        }
    }
    const LhatType *right_instance = chk_instance_of(right);
    if (right_instance != NULL) {
        for (const LhatTypeMember *m = right_instance->v.table.members;
             m != NULL; m = m->next) {
            const LhatTypeMember *shared =
                lhat_type_own_member(definition, m->name, m->name_length);
            const LhatTypeMember *held =
                lhat_type_own_member(instance, m->name, m->name_length);
            // A method is in both tables. Mirror the result rather than
            // checking it twice and reporting the same mismatch twice -- but
            // only over the method's own mirror. A name the left instance
            // holds that is not a method is a field, declared or given, and
            // only the instance holds a field: the definition's side never
            // met it, so nothing has asked whether a method may stand there.
            if (shared != NULL && !m->instance_field &&
                member_takes_receiver(m->type) &&
                (held == NULL || !held->instance_field)) {
                copy_member(c, instance, shared);
            } else {
                compose_member(c, node, instance, m, true);
            }
        }
    }

    definition->v.table.new_overridden = left->v.table.new_overridden ||
                                         right->v.table.new_overridden;
    instance->v.table.new_overridden = definition->v.table.new_overridden;

    // [syntax.obj.compose] composes to make something new, so the constructor has to build
    // the composed instance. The parameters come from the right, which is the
    // more derived of the two -- every way of constructing it ([syntax.obj.override]'s
    // overload^ may have put several there) rebuilt to answer with what this
    // composition makes.
    const LhatTypeMember *inherited = chk_find_member(right, "new", 3);
    if (inherited == NULL) {
        inherited = chk_find_member(left, "new", 3);
    }
    set_member(c, definition, "new", 3,
               constructor_from(c, inherited != NULL ? inherited->type : NULL,
                                instance));
    return definition;
}

// [syntax.obj.abstract-same]: whether this def^ also writes a value under the name, which
// is what makes a declaration of it pointless. Read off the entries rather
// than kept as a mark, since the question is asked only where an abstract^
// stands and the list is a handful of members long.
static bool defined_in_def(Checker *c, const LhatNode *node, const char *name,
                           size_t length)
{
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        if (entry->v.entry.key == NULL || entry->v.entry.declared) {
            continue;
        }
        const char *other = NULL;
        size_t other_length = 0;
        if (!chk_node_name(c, entry->v.entry.key, &other, &other_length)) {
            continue;
        }
        if (other_length == length && memcmp(other, name, length) == 0) {
            return true;
        }
    }
    return false;
}

// [syntax.obj.self]: whether this member is a method -- one that wrote self^ among
// its parameters, and so is handed a receiver. A member without it is
// static, and the name self^ means nothing inside one.
static bool declares_self(Checker *c, const LhatNode *value)
{
    if (value == NULL || value->kind != LHAT_NODE_FUNC) {
        return false;
    }
    for (const LhatNode *param = value->v.func.params; param != NULL;
         param = param->next) {
        if (chk_self_marker_at(c, value->v.func.params, param) != 0) {
            return true;
        }
    }
    return false;
}

// [syntax.obj.members-order] with [syntax.obj.self]: whether an instance carries this member. What may be
// reached through one is what is handed a receiver, and a signature says so --
// [syntax.op.right]'s operator form writes self^ second, which takes_self covers either
// way. Everything else (new, a static member, a value) is the definition's.
//
// [syntax.obj.override]'s overloaded name is carried when any arm takes a receiver: which arm
// a call means is settled at the call, and an arm that takes none is not
// reachable through an instance whatever the type says here.
bool chk_takes_receiver(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (chk_takes_receiver(arm->type)) {
                return true;
            }
        }
        return false;
    }
    return type->kind == LHAT_TYPE_FUNC && type->v.func.takes_self;
}

// [syntax.obj.delegate]: what one entry of a def^ left behind, kept from one walk of
// the entries to the next.
//
// `found` and `member` are what the first walk found under the entry's own
// name. A later walk asks [syntax.obj.override] the same question, but by then the answer has
// been written over -- by the walk itself, and by the re-seeding that hands
// the next walk what the last one learned -- so it is kept here rather than
// looked up again. A copy: the member it was taken from is the one being
// written over.
//
// `inferred` is the type the last walk gave the entry, which is how
// [pipeline.infer.rounds] tells a walk that learned something from one that only said the same
// again.
typedef struct {
    bool found;
    LhatTypeMember member;
    LhatType *inferred;
} SeenMember;

LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base)
{
    LhatDefinition *origin = lhat_definition_new(c->result->types, c->result->module_name);
    if (origin != NULL) {
        origin->literal = node;
        origin->lexer = c->lexer;
        origin->statements = c->unit != NULL ? c->unit->v.list.items : NULL;
    }
    ((LhatNode *)node)->checked_definition = origin;
    LhatType *definition = lhat_type_table(c->result->types);
    LhatType *instance = lhat_type_table(c->result->types);
    // [syntax.obj.compose]: '..' between two definitions is composition, never a call of an
    // op^.. one of them carries -- this is what tells a definition from an
    // instance of it.
    definition->v.table.is_definition = true;
    // [syntax.obj.members-order]: and this is what its instances carry. Held on the definition so
    // that [syntax.type.self]'s Self^ and the writer's self^{ … } section both read one
    // place, rather than going through what new answers.
    definition->v.table.instance = instance;
    // [syntax.stmt.member]: and both sides are closed to a member being added afterwards.
    definition->v.table.from_definition = true;
    instance->v.table.from_definition = true;

    // [syntax.obj.compose]: composition is ordered, and the derived side is written against
    // what the base already provides.
    definition->v.table.new_overridden = base != NULL && base->v.table.new_overridden;
    instance->v.table.new_overridden = definition->v.table.new_overridden;
    copy_members(c, definition, base);
    copy_members(c, instance, chk_instance_of(base));
    // [syntax.obj.delegate]: and what the base delegates to, which is a link and so
    // carries over whole. A part declaring its own writes over it below
    // -- that is the "last part wins" rule, and with one link to hold it
    // there is nothing of an earlier part's to undo.
    LhatType *base_instance = chk_instance_of(base);
    if (base_instance != NULL) {
        instance->v.table.delegate = base_instance->v.table.delegate;
    }

    // [syntax.obj.new-default]: the new every definition has is put here, before anything is
    // written -- so a written one is a second member of that name and [syntax.obj.override]
    // asks for its marker. It also replaces the base's, which answered with
    // the base's instance. What it is at run time has not changed: the
    // compiler has always written a real closure under this name.
    set_member(c, definition, "new", 3, constructor_for(c, base, instance));

    // [syntax.type.self] with [syntax.obj.instance]: writing the name of a definition asks for an instance
    // of it, so Self^ inside one names the instance as well -- the same object
    // self^ is bound to below. A definition written with no binding to take a
    // name from ([syntax.obj.name]) has this and nothing else to say its own type with.
    struct SelfLink self_here = { instance, c->self_link };
    c->self_link = &self_here;

    // [syntax.obj.name]: and the name this one is landing in, if check_define found one,
    // means the same instance from here on.
    if (c->def_link != NULL && c->def_link->node == node) {
        c->def_link->instance = instance;
    }

    const LhatNode *template = NULL;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        // [syntax.obj.delegate]: the delegate entry carries no key either, so what marks
        // the template is the absence of both.
        if (entry->v.entry.key == NULL &&
            entry->v.entry.modifier != LHAT_DEF_DELEGATE) {
            template = entry->v.entry.value;
        }
    }

    // [syntax.obj.self]: def^ and Self^ name the same definition in value position,
    // and every member reaches it, including static members: the definition
    // exists before any instance. self^ is bound per member below: only
    // one that wrote it among its parameters is handed a receiver, so only
    // there does the name mean anything.
    Scope members;
    chk_scope_open(&members, c->scope, NULL, false);
    const char *owner_names[] = { "def^", "Self^" };
    for (size_t i = 0; i < 2; i++) {
        Binding *owner = chk_scope_add(&members, owner_names[i], strlen(owner_names[i]),
                                       definition, node->offset);
        if (owner != NULL) {
            owner->reached = true;
            owner->declaration = node;
        }
    }

    Scope *outer = c->scope;
    c->scope = &members;

    // [syntax.obj.members-order]: the names first, so the members can reach each other however
    // they are ordered -- 'expr calls term calls factor calls expr' is a ring
    // and no ordering unties it. What can be put down here is what can be
    // read without walking a body: a declaration's written type, and a
    // subroutine's written signature. A member whose type only its value
    // knows is not seeded, which is [pipeline.infer]'s line -- what is read before
    // it is inferred has to have been written.
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        // [syntax.annot.where]: a member of a def^ takes one.
        chk_check_annotations(c, entry->v.entry.annotations,
                              LHAT_ANNOTATION_MEMBER);
        chk_refuse_hat_name(c, entry->v.entry.key, true);
        if (entry->v.entry.key == NULL ||
            !chk_node_name(c, entry->v.entry.key, &name, &length)) {
            continue;
        }
        // A declaration's type is read here and nowhere else -- reading it
        // twice would report whatever is wrong with it twice.
        LhatType *seed = entry->v.entry.declared
                             ? chk_resolve_type(c, entry->v.entry.value)
                             : entry->v.entry.type != NULL
                                   ? chk_resolve_type(c, entry->v.entry.type)
                             : chk_declared_signature(c, entry->v.entry.value);
        if (seed == NULL) {
            continue;
        }
        chk_member_declared_at(
            c,
            set_member_provisional(c, definition, name, length, seed,
                                   entry->v.entry.declared),
            entry->v.entry.key);
        // [syntax.obj.members-order]: an instance carries what may be reached through one, which
        // the signature says. A declaration ([syntax.obj.abstract]) is read the same way --
        // what fills it in is held to the type written here.
        if (chk_takes_receiver(seed)) {
            chk_member_declared_at(
                c,
                set_member_provisional(c, instance, name, length, seed,
                                       entry->v.entry.declared),
                entry->v.entry.key);
        }
    }

    // The fields next, so a member body sees them through self^. Inside the
    // members scope: [syntax.obj.new] evaluates an initialiser once, as the definition
    // is built, where def^ and every member are in place -- self^ is not,
    // since no instance exists yet, and the scope holds no such name.
    if (template != NULL) {
        for (const LhatNode *field = template->v.list.items; field != NULL;
             field = field->next) {
            const char *name = NULL;
            size_t length = 0;
            // [syntax.annot.where]: a field of a self^{ … } takes one, which is where
            // an @export-shaped annotation goes.
            chk_check_annotations(c, field->v.entry.annotations,
                                  LHAT_ANNOTATION_FIELD);
            chk_refuse_hat_name(c, field->v.entry.key, true);
            if (!chk_node_name(c, field->v.entry.key, &name, &length)) {
                continue;
            }
            const LhatTypeMember *required =
                lhat_type_own_member(instance, name, length);
            if (required != NULL && !required->instance_field) {
                chk_report_named(c, field, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                                 name, length);
                continue;
            }
            // [syntax.obj.abstract]: a field the composition has to provide carries its type
            // and no value -- and so no key on the prototype.
            if (field->v.entry.declared) {
                LhatType *declared =
                    chk_resolve_type(c, field->v.entry.value);
                if (required != NULL && required->abstract) {
                    declared = lhat_type_intersect(c->result->types,
                                                  required->type, declared);
                }
                chk_check_tostring_shape(c, field, declared, name, length, true);
                LhatTypeMember *added =
                    set_member_as(c, instance, name, length, declared, true);
                if (added != NULL) { added->instance_field = true; }
                chk_member_declared_at(c, added, field->v.entry.key);
                continue;
            }
            // [syntax.obj.template]: a type written alongside the value is what the field
            // holds, and the value is measured against it -- the same reading
            // [modules.host.machine]'s 'let^ x : T = v' has, and for the same reason: what a
            // reader may rely on is what was written, not what the first
            // value happened to be.
            LhatType *written = chk_resolve_type(c, field->v.entry.type);
            LhatType *outer_expected = c->expected_func;
            c->expected_func = written;
            LhatType *actual = chk_infer(c, field->v.entry.value);
            c->expected_func = outer_expected;
            if (written != NULL) {
                chk_expect(c, field->v.entry.value, actual, written,
                           LHAT_CHECK_ERR_MISMATCH);
            }
            if (required != NULL && required->abstract) {
                chk_expect(c, field, written != NULL ? written : actual,
                           required->type, LHAT_CHECK_ERR_MISMATCH);
            }
            // [syntax.obj.new]: the value goes on the prototype and every instance
            // starts as a copy of it, so it has to be a leaf nothing can
            // write through or a literal tree the copy owns. The value's own
            // type and spelling are what is judged -- 'slot : any^ = 1'
            // shares a number, however wide the field is. The machine asks
            // the same question of the values themselves (SETPROTO), for the
            // code checking never saw.
            if (!immutable_default_value(c, field->v.entry.value, actual)) {
                chk_report_named(c, field->v.entry.value,
                                 LHAT_CHECK_ERR_MUTABLE_DEFAULT, name, length);
            }
            chk_check_tostring_shape(c, field, written != NULL ? written : actual,
                                     name, length, true);
            LhatTypeMember *added = set_member(
                c, instance, name, length, written != NULL ? written : actual);
            if (added != NULL) { added->instance_field = true; }
            chk_member_declared_at(c, added, field->v.entry.key);
        }
    }

    // [pipeline.infer.rounds]: the walk below is one iteration of a least fixpoint. A
    // member read before its own body was walked answers with a seed, and a
    // ring of them -- expr calls term calls factor calls expr -- has at least
    // one such read whatever the order. Walking the entries again from what
    // the last walk inferred is what closes the ring, and it is walked again
    // only while that keeps changing what they answer. Rounds holds the
    // bookkeeping; what is seeded and what is put back before a walk is here.
    size_t entry_count = 0;
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        entry_count++;
    }
    SeenMember *seen =
        entry_count > 0 ? calloc(entry_count, sizeof(*seen)) : NULL;
    Rounds rounds;
    chk_rounds_begin(c, &rounds, seen != NULL ? entry_count : 0);

    do {
        // [syntax.obj.delegate]: before the bodies, so a member reaches through self^ what
        // this definition delegates to -- the whole point of writing one is
        // that the name is the wrapper's own, and that has to hold inside it
        // as much as outside. Silent here: the round it settles in is not
        // known ahead, and what a round says is thrown away when another
        // follows (chk_rounds_next), so the last one is the one that reports.
        link_delegate(c, node, definition, instance, false);

        size_t round = rounds.round;
        size_t index = 0;
        for (const LhatNode *entry = node->v.list.items; entry != NULL;
             entry = entry->next, index++) {
            const char *name = NULL;
            size_t length = 0;
            if (entry->v.entry.key == NULL ||
                !chk_node_name(c, entry->v.entry.key, &name, &length)) {
                continue;
            }
            // [syntax.obj.override] asks what was already under this name, and a seed from
            // the pass above was not -- it is this very entry, read ahead.
            const LhatTypeMember *hidden = NULL;
            if (round == 0) {
                hidden = chk_find_member(definition, name, length);
                if (hidden != NULL && hidden->provisional) {
                    hidden = NULL;
                }
                if (seen != NULL) {
                    seen[index].found = hidden != NULL;
                    if (hidden != NULL) {
                        seen[index].member = *hidden;
                    }
                }
            } else if (seen != NULL && seen[index].found) {
                // [syntax.obj.delegate]: asking again would answer with what this very walk
                // wrote over the name, so the first walk's answer is kept
                // above and handed back here.
                hidden = &seen[index].member;
            }

            // [syntax.obj.abstract]: a declaration carries a type and no value, and says the
            // composition has to provide the member. It is not a definition
            // of it, so [syntax.obj.override] has nothing to check here.
            if (entry->v.entry.declared) {
                const LhatTypeMember *seeded =
                    chk_find_member(definition, name, length);
                LhatType *declared =
                    seeded != NULL && seeded->provisional
                        ? seeded->type
                        : chk_resolve_type(c, entry->v.entry.value);
                const LhatTypeMember *held =
                    lhat_type_own_member(instance, name, length);
                if (chk_takes_receiver(declared) && held != NULL &&
                    held->instance_field) {
                    chk_report_named(c, entry, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                                     name, length);
                    continue;
                }
                // [syntax.op.functions]: an op^ is a member with an operator for a name, and
                // declaring one asks the same shape of it that giving one does.
                if (chk_is_operator_name(name, length)) {
                    chk_check_operator_shape(c, entry, declared, name, length);
                } else {
                    chk_refuse_self_last(c, entry, declared);
                }
                chk_check_tostring_shape(c, entry, declared, name, length, true);
                if (hidden != NULL && !hidden->abstract) {
                    // Already provided, so the declaration asks for nothing.
                    chk_report(c, entry, LHAT_CHECK_ERR_ALREADY_PROVIDED);
                }
                if (hidden != NULL && hidden->abstract) {
                    declared = lhat_type_intersect(c->result->types,
                                                  hidden->type, declared);
                }
                // [syntax.obj.abstract-same]: and the composition is where it is provided.
                // Writing the value here as well leaves the declaration
                // saying nothing -- [syntax.obj.members-order] already lets the members reach
                // each other whatever the order, so there is no forward
                // reference left to declare for.
                if (defined_in_def(c, node, name, length)) {
                    chk_report(c, entry, LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE);
                }
                chk_member_declared_at(
                    c,
                    set_member_as(c, definition, name, length, declared, true),
                    entry->v.entry.key);
                if (chk_takes_receiver(declared)) {  // [syntax.obj.members-order], as above
                    chk_member_declared_at(
                        c,
                        set_member_as(c, instance, name, length, declared,
                                      true),
                        entry->v.entry.key);
                }
                continue;
            }

            // [syntax.obj.new]: a member spelled new and written as a body is the
            // constructor. The machine makes the instance and the body only
            // adjusts it -- so self^ is bound around it the way a method's
            // is, and chk_infer_func reads c->new_func to open the body as
            // one.
            if (chk_name_is(name, length, "new") &&
                entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                definition->v.table.new_overridden = true;
                instance->v.table.new_overridden = true;
            }
            bool constructor_entry =
                chk_name_is(name, length, "new") &&
                entry->v.entry.value != NULL &&
                entry->v.entry.value->kind == LHAT_NODE_FUNC;

            // [syntax.obj.super]: super^ is a name only inside an override^, and what it
            // names is everything that was under this name -- the whole
            // intersection when [syntax.obj.override]'s overload^ put several there, since
            // the write replaces the member as a whole.
            LhatType *outer_super = c->super_type;
            const LhatNode *outer_super_binding = c->super_binding;
            c->super_type = NULL;
            c->super_binding = NULL;
            if (entry->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                // [syntax.obj.abstract-override]: with nothing under the name yet, the shape super^
                // will have is the one written here. [syntax.obj.override] has the
                // replacement usable where the original was -- arguments
                // wider, result narrower -- so what is written is admissible
                // wherever the original is, and taking it for super^ cannot
                // promise more than the base gives.
                c->super_binding = entry;
                c->super_type =
                    hidden != NULL
                        ? hidden->type
                        : chk_declared_signature(c, entry->v.entry.value);
                // [syntax.obj.new]: inside a new the name means the hook of the new
                // before it -- the same arguments run against the same
                // receiver, with nothing to answer.
                if (constructor_entry) {
                    c->super_type = hook_signature(c, c->super_type);
                }
            }
            // [syntax.obj.self]: the receiver is this member's, so it is bound around this
            // body and nowhere else. A subroutine written inside the body
            // still reaches it, the way any name in scope is reached ([syntax.if.bool]'s
            // capture).
            Scope receiver;
            bool method = declares_self(c, entry->v.entry.value) ||
                          constructor_entry;
            if (method) {
                chk_scope_open(&receiver, c->scope, NULL, false);
                Binding *bound = chk_scope_add(&receiver, "self^", 5, instance,
                                               node->offset);
                if (bound != NULL) {
                    bound->reached = true;
                    bound->declaration = entry->v.entry.value;
                }
                c->scope = &receiver;
            }
            const LhatNode *outer_new = c->new_func;
            if (constructor_entry) {
                c->new_func = entry->v.entry.value;
            }
            LhatType *annotation = entry->v.entry.type != NULL
                                       ? chk_resolve_type(c, entry->v.entry.type) : NULL;
            LhatType *outer_expected = c->expected_func;
            c->expected_func = annotation;
            LhatType *type = chk_infer(c, entry->v.entry.value);
            c->expected_func = outer_expected;
            if (annotation != NULL) {
                chk_expect(c, entry->v.entry.value, type, annotation, LHAT_CHECK_ERR_MISMATCH);
                type = annotation;
            }
            c->new_func = outer_new;
            if (method) {
                c->scope = &members;
                chk_scope_close(c, &receiver);
            }
            c->super_type = outer_super;
            c->super_binding = outer_super_binding;
            const LhatTypeMember *held =
                lhat_type_own_member(instance, name, length);
            if (chk_takes_receiver(type) && held != NULL && held->instance_field) {
                chk_report_named(c, entry, LHAT_CHECK_ERR_FIELD_METHOD_COLLISION,
                                 name, length);
                continue;
            }
            // [syntax.obj.override]: two members of one name in a single def^ need a marker
            // too, so what is already there has to include this def^'s
            // earlier entries and not only what the base brought.
            // `definition` is both -- it was copied from the base above and
            // has been accumulating since.
            type = check_same_name(c, entry, hidden, type,
                                   chk_name_is(name, length, "new"));
            if (chk_is_operator_name(name, length)) {
                chk_check_operator_shape(c, entry, type, name, length);
            } else {
                chk_refuse_self_last(c, entry, type);
            }
            chk_check_tostring_shape(c, entry, type, name, length, true);
            // [syntax.obj.abstract-override]: an override^ that found nothing waits for a
            // composition to bring what it replaces. Until then super^ inside
            // it points at nothing, so [syntax.obj.new]'s new has to stay out of reach.
            // Landing on another one that is waiting settles neither.
            bool pending = entry->v.entry.modifier == LHAT_DEF_OVERRIDE &&
                           (hidden == NULL || hidden->pending);
            // [syntax.obj.abstract]: what answers an abstract^ keeps saying so, for the
            // composition that might make the name ambiguous later.
            bool demanded =
                hidden != NULL && (hidden->abstract || hidden->demanded);
            LhatTypeMember *written = set_member_marked(
                c, definition, name, length, type, false, pending);
            if (written != NULL) {
                written->demanded = written->demanded || demanded;
            }
            chk_member_declared_at(c, written, entry->v.entry.key);
            // [syntax.obj.members-order]: only what is handed a receiver is reachable through an
            // instance. new and a static member stay the definition's.
            if (chk_takes_receiver(type)) {
                LhatTypeMember *mirrored = set_member_marked(
                    c, instance, name, length, type, false, pending);
                if (mirrored != NULL) {
                    mirrored->demanded = mirrored->demanded || demanded;
                }
                chk_member_declared_at(c, mirrored, entry->v.entry.key);
            }
            if (seen != NULL) {
                if (round == 0 || !lhat_type_equal(seen[index].inferred, type)) {
                    rounds.changed = true;
                }
                seen[index].inferred = type;
            }
        }

        if (!chk_rounds_next(c, &rounds)) {
            break;
        }
        // What this def^ wrote goes back to being a seed, carrying the type
        // the walk just inferred -- that is what starting from what was
        // learned amounts to. A member the base brought keeps what the walk
        // made of it: the entry hiding it was answered from `seen` above.
        for (const LhatNode *entry = node->v.list.items; entry != NULL;
             entry = entry->next) {
            const char *name = NULL;
            size_t length = 0;
            if (entry->v.entry.key == NULL ||
                !chk_node_name(c, entry->v.entry.key, &name, &length)) {
                continue;
            }
            mark_provisional(definition, name, length);
            mark_provisional(instance, name, length);
        }
    } while (true);
    free(seen);
    chk_rounds_end(c, &rounds);

    c->scope = outer;
    chk_scope_close(c, &members);

    // [syntax.obj.abstract-new]: a template field the written new gives a value to is provided,
    // the same as one a composition fills. [syntax.obj.override-new] makes an override^ new
    // replace the default whole, so that new is the only way to build one and
    // what it writes is what every instance has. An overload^ leaves the
    // default arm standing, and that one writes nothing -- so it settles
    // nothing here.
    for (const LhatNode *entry = node->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        if (entry->v.entry.modifier != LHAT_DEF_OVERRIDE ||
            entry->v.entry.key == NULL ||
            !chk_node_name(c, entry->v.entry.key, &name, &length) ||
            !chk_name_is(name, length, "new")) {
            continue;
        }
        // The body, not the f^ itself: collect_written_fields stops at a
        // subroutine, which is what keeps a nested one's self^{ … } out.
        const LhatNode *made = entry->v.entry.value;
        if (made == NULL || made->kind != LHAT_NODE_FUNC) {
            break;
        }
        WrittenFields written;
        written.c = c;
        written.count = 0;
        written.seen_one = false;
        written.overflowed = false;
        collect_written_fields(&written, made->v.func.body);
        if (written.overflowed) {
            break;  // more than can be tracked; leave every hole as it was
        }
        for (size_t i = 0; i < written.count; i++) {
            for (LhatTypeMember *m = instance->v.table.members; m != NULL;
                 m = m->next) {
                if (m->abstract && m->name_length == written.lengths[i] &&
                    memcmp(m->name, written.names[i], m->name_length) == 0) {
                    m->abstract = false;
                    break;
                }
            }
        }
        break;  // [syntax.obj.override] allows one entry of the name, so there is one to read
    }

    // [syntax.obj.delegate]: and again once every round is over, which is where a delegate
    // that never settled is reported. Writing the same link twice is
    // writing it once.
    link_delegate(c, node, definition, instance, true);

    c->self_link = self_here.outer;
    return definition;
}

// ---------------------------------------------------------------------------
// [pipeline.infer.expected]: the operators this unit writes
// ---------------------------------------------------------------------------

// Which operators one def^ writes, following [syntax.obj.compose]'s '..' so that a chain
// carries what its parts wrote. Composing is settled at the definition
// ([syntax.obj.chain]), so the parts are here to read.
static uint16_t operators_written(Checker *c, const LhatNode *definition)
{
    if (definition == NULL) {
        return 0;
    }
    if (definition->kind == LHAT_NODE_BINARY &&
        definition->v.binary.op == LHAT_OP_CONCAT) {
        return (uint16_t)(operators_written(c, definition->v.binary.left) |
                          operators_written(c, definition->v.binary.right));
    }
    if (definition->kind != LHAT_NODE_DEF) {
        return 0;
    }
    uint16_t written = 0;
    for (const LhatNode *entry = definition->v.list.items; entry != NULL;
         entry = entry->next) {
        const char *name = NULL;
        size_t length = 0;
        if (entry->v.entry.key != NULL &&
            chk_node_name(c, entry->v.entry.key, &name, &length)) {
            written |= chk_operator_bit(name, length);
        }
    }
    return written;
}

// [modules.host.import]: the operators one registered type carries. A host value keeps
// them in the same member list a table keeps [syntax.op.define]'s ([syntax.stmt.let]), and a hostdata
// type is a nominal table, so both are read the same way.
static uint16_t operators_registered(Checker *c, const LhatType *type)
{
    (void)c;
    if (type == NULL ||
        (type->kind != LHAT_TYPE_HOSTVALUE &&
         !(type->kind == LHAT_TYPE_TABLE && type->v.table.nominal))) {
        return 0;
    }
    uint16_t written = 0;
    for (const LhatTypeMember *m = type->v.table.members; m != NULL;
         m = m->next) {
        written |= chk_operator_bit(m->name, m->name_length);
    }
    return written;
}

// One carrier, however it is to be found again later.
static void keep_carrier(Checker *c, const char *name, size_t length,
                         const LhatType *type, uint16_t operators)
{
    LHAT_GROW(c->operator_carriers, c->operator_carrier_count,
              c->operator_carrier_capacity, 8, return);
    OperatorCarrier *at = &c->operator_carriers[c->operator_carrier_count++];
    at->name = name;
    at->name_length = length;
    at->type = type;
    at->operators = operators;
    c->unit_operators |= operators;
}

// [modules.host.import]: what an import^ brought within reach -- what the one unit it
// names published, and nothing of the units named under it, which come in
// by an import^ of their own.
static void collect_registered_carriers(Checker *c, const LhatType *module)
{
    if (module == NULL || module->kind != LHAT_TYPE_TABLE) {
        return;
    }
    for (const LhatTypeMember *m = module->v.table.members; m != NULL;
         m = m->next) {
        uint16_t written = operators_registered(c, m->type);
        if (written != 0) {
            keep_carrier(c, m->name, m->name_length, m->type, written);
        }
    }
}

// The path an import^ statement names, in either of its two spellings --
// 'import^ a.b' on its own, and 'let^ m = import^ a.b'.
static const LhatNode *imported_path(const LhatNode *statement)
{
    if (statement->kind == LHAT_NODE_IMPORT_STMT) {
        return statement->v.jump.value;
    }
    if (statement->kind == LHAT_NODE_DEFINE &&
        statement->v.binding.values != NULL &&
        statement->v.binding.values->kind == LHAT_NODE_IMPORT) {
        return statement->v.binding.values->v.jump.value;
    }
    return NULL;
}

// [pipeline.infer.expected]: read before anything is walked. [modules.host.import] makes a name visible over
// the whole unit, so a body using an operator has to see the same candidates
// wherever the def^ carrying one stands relative to it -- which is why this
// is a pass of its own rather than something the walk accumulates.
//
// Only the top level, and only what this unit can name. A def^ made inside a
// body is not a name any other body here can write, and [modules.require.once]'s require^
// reaches another unit -- a signature written here should be readable from
// here. [modules.host.import]'s registry is on the other side of that line: what an import^
// brought in is written in this very unit, and the registry does not move
// with the order units are checked in ([modules.host.import]), so it counts.
void chk_collect_operator_carriers(Checker *c, const LhatNode *statements)
{
    for (const LhatNode *s = statements; s != NULL; s = s->next) {
        const LhatNode *path = imported_path(s);
        if (path != NULL) {
            collect_registered_carriers(c, chk_hosted_module(c, path));
            continue;
        }
        if (s->kind != LHAT_NODE_DEFINE || s->v.binding.values == NULL) {
            continue;
        }
        uint16_t written = operators_written(c, s->v.binding.values);
        if (written == 0) {
            continue;
        }
        const LhatNode *target = s->v.binding.targets;
        const char *name = NULL;
        size_t length = 0;
        if (target == NULL || target->next != NULL ||
            !chk_node_name(c, lhat_define_target_name(target), &name,
                           &length)) {
            continue;  // [syntax.type.destructure]'s several names take a run apart, not a def^
        }
        keep_carrier(c, name, length, NULL, written);
    }
}

void chk_dispose_operator_carriers(Checker *c)
{
    lhat_free(c->operator_carriers);
    c->operator_carriers = NULL;
    c->operator_carrier_count = 0;
    c->operator_carrier_capacity = 0;
    c->unit_operators = 0;
}
