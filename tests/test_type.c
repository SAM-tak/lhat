// L^ (lhat) -- tests for the type relations.
//
// Section numbers refer to DesignDocuments/02-syntax.md unless prefixed with
// "04". The cases worth pinning are the ones where a decision in the
// specification would otherwise be invisible: that a structure asks for at
// least its members (14.10), that two structures overlap unless a shared name
// conflicts (14.12), and that an error kind's identity is its declaration
// rather than its shape (04 の 2.4).

#include <string.h>

#include "testutil.h"
#include "type.h"

typedef struct {
    LhatTypeArena arena;
} Types;

static void types_init(Types *t)
{
    lhat_type_arena_init(&t->arena);
}

static void types_dispose(Types *t)
{
    lhat_type_arena_dispose(&t->arena);
}

static LhatType *simple(Types *t, LhatTypeKind kind)
{
    return lhat_type_simple(&t->arena, kind);
}

// Structures written as t^{ ... } with nought, one or two members.
static LhatType *table0(Types *t)
{
    return lhat_type_table(&t->arena);
}

static LhatType *table1(Types *t, const char *name, LhatType *type)
{
    LhatType *s = lhat_type_table(&t->arena);
    lhat_type_add_member(&t->arena, s, name, strlen(name), type);
    return s;
}

static LhatType *table2(Types *t, const char *a, LhatType *at,
                        const char *b, LhatType *bt)
{
    LhatType *s = table1(t, a, at);
    lhat_type_add_member(&t->arena, s, b, strlen(b), bt);
    return s;
}

static void test_primitives(void)
{
    Types t;
    types_init(&t);

    LHAT_TEST("a primitive conforms only to itself");
    {
        LhatType *n = simple(&t, LHAT_TYPE_NUMBER);
        LhatType *s = simple(&t, LHAT_TYPE_STRING);
        LHAT_CHECK(lhat_type_conforms(n, simple(&t, LHAT_TYPE_NUMBER)),
                   "number^ fits number^");
        LHAT_CHECK(!lhat_type_conforms(n, s), "number^ does not fit string^");
        LHAT_CHECK(lhat_type_disjoint(n, s), "different primitives are separate");
        LHAT_CHECK(!lhat_type_disjoint(n, simple(&t, LHAT_TYPE_NUMBER)),
                   "the same primitive is not separate from itself");
    }

    // 13.7: any^ is the top of every value, not only of tables. Identifying it
    // with t^{} would have left number^ outside it.
    LHAT_TEST("any^ admits every value");
    {
        LhatType *any = simple(&t, LHAT_TYPE_ANY);
        LHAT_CHECK(lhat_type_conforms(simple(&t, LHAT_TYPE_NUMBER), any),
                   "a number fits any^");
        LHAT_CHECK(lhat_type_conforms(table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER)),
                                      any),
                   "a table fits any^");
        LHAT_CHECK(lhat_type_conforms(simple(&t, LHAT_TYPE_ERROR), any),
                   "an error fits any^");
        LHAT_CHECK(!lhat_type_disjoint(any, simple(&t, LHAT_TYPE_STRING)),
                   "any^ is separate from nothing");
    }

    // An empty structure is the top of tables only, which is exactly why it
    // could not serve as any^.
    LHAT_TEST("t^{} is not the top of every value");
    {
        LhatType *empty = table0(&t);
        LHAT_CHECK(lhat_type_conforms(table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER)),
                                      empty),
                   "every table fits t^{}");
        LHAT_CHECK(!lhat_type_conforms(simple(&t, LHAT_TYPE_NUMBER), empty),
                   "a number does not fit t^{}");
    }

    // 03 の 3.5: a gap in inference is not a mismatch, so it never cascades.
    LHAT_TEST("unknown does not report against anything");
    {
        LhatType *unknown = simple(&t, LHAT_TYPE_UNKNOWN);
        LHAT_CHECK(lhat_type_conforms(unknown, simple(&t, LHAT_TYPE_NUMBER)),
                   "unknown fits anything");
        LHAT_CHECK(lhat_type_conforms(simple(&t, LHAT_TYPE_NUMBER), unknown),
                   "anything fits unknown");
        LHAT_CHECK(!lhat_type_disjoint(unknown, simple(&t, LHAT_TYPE_STRING)),
                   "unknown rules nothing out");
    }

    types_dispose(&t);
}

// 14.10.
static void test_structures(void)
{
    Types t;
    types_init(&t);

    LHAT_TEST("a structure asks for at least its members");
    {
        LhatType *want = table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER));
        LhatType *has_more = table2(&t, "a", simple(&t, LHAT_TYPE_NUMBER),
                                    "b", simple(&t, LHAT_TYPE_STRING));
        LHAT_CHECK(lhat_type_conforms(has_more, want), "extra members are fine");
        LHAT_CHECK(!lhat_type_conforms(want, has_more), "a missing member is not");
    }

    LHAT_TEST("a member's type has to fit too");
    {
        LhatType *want = table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER));
        LhatType *wrong = table1(&t, "a", simple(&t, LHAT_TYPE_STRING));
        LHAT_CHECK(!lhat_type_conforms(wrong, want), "a is the wrong type");
    }

    // 11.3 and 14.9: identity is the shape, so nothing has to be shared for
    // two independently built structures to be the same type.
    LHAT_TEST("identity is structural");
    {
        LhatType *a = table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER));
        LhatType *b = table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER));
        LHAT_CHECK(a != b, "two separate objects");
        LHAT_CHECK(lhat_type_equal(a, b), "and one type");
    }

    // 14.12: this is why overloading on shape is refused. A value can carry
    // both sets of members, so neither signature is the more specific one.
    LHAT_TEST("structures sharing no name are not separate");
    {
        LhatType *radius = table1(&t, "radius", simple(&t, LHAT_TYPE_NUMBER));
        LhatType *width = table1(&t, "width", simple(&t, LHAT_TYPE_NUMBER));
        LHAT_CHECK(!lhat_type_disjoint(radius, width),
                   "a value may have both members");
    }

    LHAT_TEST("a shared name with conflicting types separates them");
    {
        LhatType *as_number = table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER));
        LhatType *as_string = table1(&t, "a", simple(&t, LHAT_TYPE_STRING));
        LHAT_CHECK(lhat_type_disjoint(as_number, as_string),
                   "a cannot be both");
    }

    LHAT_TEST("a structure is separate from a primitive");
    {
        LHAT_CHECK(lhat_type_disjoint(table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER)),
                                      simple(&t, LHAT_TYPE_NUMBER)),
                   "a table is not a number");
    }

    types_dispose(&t);
}

// 13.5 and 14.5.
static void test_composites(void)
{
    Types t;
    types_init(&t);

    // 04 の 8.1: this is the whole detection mechanism for an unhandled
    // error. A union has to fit as a whole, so one bad arm is enough.
    LHAT_TEST("every arm of a union has to fit");
    {
        LhatType *number = simple(&t, LHAT_TYPE_NUMBER);
        LhatType *maybe = lhat_type_union(&t.arena, number,
                                          simple(&t, LHAT_TYPE_ERROR));
        LHAT_CHECK(!lhat_type_conforms(maybe, simple(&t, LHAT_TYPE_NUMBER)),
                   "number^|error^ does not fit number^");
        LHAT_CHECK(lhat_type_conforms(number, maybe),
                   "but number^ fits number^|error^");
    }

    LHAT_TEST("a redundant arm collapses");
    {
        LhatType *once = lhat_type_union(&t.arena, simple(&t, LHAT_TYPE_NUMBER),
                                         simple(&t, LHAT_TYPE_NUMBER));
        LHAT_CHECK_EQ_INT(once->kind, LHAT_TYPE_NUMBER);
    }

    // 13.7: any^ admits everything, so a union with it is it.
    LHAT_TEST("a union with any^ is any^");
    {
        LhatType *u = lhat_type_union(&t.arena, simple(&t, LHAT_TYPE_NUMBER),
                                      simple(&t, LHAT_TYPE_ANY));
        LHAT_CHECK_EQ_INT(u->kind, LHAT_TYPE_ANY);
    }

    LHAT_TEST("unions flatten");
    {
        LhatType *ab = lhat_type_union(&t.arena, simple(&t, LHAT_TYPE_NUMBER),
                                       simple(&t, LHAT_TYPE_STRING));
        LhatType *abc = lhat_type_union(&t.arena, ab, simple(&t, LHAT_TYPE_BOOL));
        LHAT_CHECK_EQ_INT(abc->kind, LHAT_TYPE_UNION);
        size_t arms = 0;
        for (const LhatTypeList *arm = abc->v.composite.arms; arm != NULL;
             arm = arm->next) {
            arms++;
        }
        LHAT_CHECK_EQ_INT(arms, 3);
    }

    LHAT_TEST("a union is separate only when all of it is");
    {
        LhatType *ns = lhat_type_union(&t.arena, simple(&t, LHAT_TYPE_NUMBER),
                                       simple(&t, LHAT_TYPE_STRING));
        LHAT_CHECK(!lhat_type_disjoint(ns, simple(&t, LHAT_TYPE_NUMBER)),
                   "number^ is one of the arms");
        LHAT_CHECK(lhat_type_disjoint(ns, simple(&t, LHAT_TYPE_BOOL)),
                   "bool^ is none of them");
    }

    // 14.5: an intersection has every structure, which is how 14.12 writes an
    // overloaded member.
    LHAT_TEST("an intersection has to satisfy every arm");
    {
        LhatType *a = table1(&t, "a", simple(&t, LHAT_TYPE_NUMBER));
        LhatType *b = table1(&t, "b", simple(&t, LHAT_TYPE_STRING));
        LhatType *both = lhat_type_intersect(&t.arena, a, b);
        LhatType *value = table2(&t, "a", simple(&t, LHAT_TYPE_NUMBER),
                                 "b", simple(&t, LHAT_TYPE_STRING));
        LHAT_CHECK(lhat_type_conforms(value, both), "the value has both");
        LHAT_CHECK(!lhat_type_conforms(a, both), "a alone does not");
        LHAT_CHECK(lhat_type_conforms(both, a), "the intersection fits either");
    }

    types_dispose(&t);
}

// 15 章 and 14.12.
static void test_functions(void)
{
    Types t;
    types_init(&t);

    LHAT_TEST("arguments may widen and results may narrow");
    {
        // p^string^ -> number^|nil^;  against  p^string^|number^ -> number^;
        LhatType *narrow = lhat_type_func(&t.arena, false);
        lhat_type_add_param(&t.arena, narrow, simple(&t, LHAT_TYPE_STRING));
        narrow->v.func.result = lhat_type_union(&t.arena,
                                                simple(&t, LHAT_TYPE_NUMBER),
                                                simple(&t, LHAT_TYPE_NIL));

        LhatType *wide = lhat_type_func(&t.arena, false);
        lhat_type_add_param(&t.arena, wide,
                            lhat_type_union(&t.arena, simple(&t, LHAT_TYPE_STRING),
                                            simple(&t, LHAT_TYPE_NUMBER)));
        wide->v.func.result = simple(&t, LHAT_TYPE_NUMBER);

        LHAT_CHECK(lhat_type_conforms(wide, narrow),
                   "wider arguments and a narrower result substitute");
        LHAT_CHECK(!lhat_type_conforms(narrow, wide), "the other way does not");
    }

    // 13.12: an f^ is a p^ held to stricter rules, so it stands wherever a
    // p^ is written. The other way round is what 15 章 rules out.
    LHAT_TEST("f^ substitutes for p^ and not the other way");
    {
        LhatType *fn = lhat_type_func(&t.arena, true);
        LhatType *proc = lhat_type_func(&t.arena, false);
        LHAT_CHECK(lhat_type_conforms(fn, proc), "f^ is a p^");
        LHAT_CHECK(!lhat_type_conforms(proc, fn), "a p^ is not an f^");
    }

    // 13.12: the word on its own is the top of its kind, which every type of
    // that kind is below.
    LHAT_TEST("the top of a kind takes everything of that kind");
    {
        LhatType *any_proc =
            lhat_type_kind_top(&t.arena, LHAT_TYPE_FUNC, false);
        LhatType *any_func = lhat_type_kind_top(&t.arena, LHAT_TYPE_FUNC, true);
        LhatType *any_cont = lhat_type_kind_top(&t.arena, LHAT_TYPE_CORO, false);

        LhatType *proc = lhat_type_func(&t.arena, false);
        proc->v.func.result = simple(&t, LHAT_TYPE_NUMBER);
        LhatType *fn = lhat_type_func(&t.arena, true);
        LhatType *cont = lhat_type_coro(&t.arena, simple(&t, LHAT_TYPE_NUMBER),
                                        simple(&t, LHAT_TYPE_STRING));

        LHAT_CHECK(lhat_type_conforms(proc, any_proc), "a p^ is below p^");
        LHAT_CHECK(lhat_type_conforms(fn, any_proc), "an f^ is below p^ too");
        LHAT_CHECK(!lhat_type_conforms(proc, any_func), "a p^ is not below f^");
        LHAT_CHECK(lhat_type_conforms(cont, any_cont), "a c^…; is below c^");
        LHAT_CHECK(!lhat_type_conforms(any_proc, proc),
                   "the top says nothing about how it may be called");
    }

    // 13.2: returning nothing is not the same as returning something.
    LHAT_TEST("a missing result is not a result");
    {
        LhatType *silent = lhat_type_func(&t.arena, false);
        LhatType *speaking = lhat_type_func(&t.arena, false);
        speaking->v.func.result = simple(&t, LHAT_TYPE_NUMBER);
        LHAT_CHECK(!lhat_type_conforms(silent, speaking), "one returns nothing");
        LHAT_CHECK(!lhat_type_conforms(speaking, silent), "the other returns");
    }

    // 12.5 と 14.10: the structural test with^ needs is exactly this.
    LHAT_TEST("with^ asks for a dispose() that returns nothing");
    {
        LhatType *dispose = lhat_type_func(&t.arena, false);  // p^self^;
        LhatType *required = table1(&t, "dispose", dispose);

        LhatType *good = table2(&t, "dispose", lhat_type_func(&t.arena, false),
                                "read", lhat_type_func(&t.arena, true));
        LHAT_CHECK(lhat_type_conforms(good, required), "it has dispose()");

        LhatType *returns_something = lhat_type_func(&t.arena, false);
        returns_something->v.func.result = simple(&t, LHAT_TYPE_NUMBER);
        LhatType *bad = table1(&t, "dispose", returns_something);
        LHAT_CHECK(!lhat_type_conforms(bad, required),
                   "12.7: dispose() must not return");
    }

    types_dispose(&t);
}

// 04 の 2.3, 2.4, 2.6.
static void test_errors(void)
{
    Types t;
    types_init(&t);

    LhatType *io = lhat_type_error_set(&t.arena, "IOError", 7);
    LhatType *not_found = lhat_type_error_kind(&t.arena, io, "NotFound", 8);
    LhatType *denied = lhat_type_error_kind(&t.arena, io, "Denied", 6);

    // 2.4: two declarations written the same way are different types. This is
    // the one place identity is not structural.
    LhatType *user = lhat_type_error_set(&t.arena, "UserError", 9);
    LhatType *user_not_found = lhat_type_error_kind(&t.arena, user, "NotFound", 8);

    LHAT_TEST("a kind is below its set and below error^");
    {
        LHAT_CHECK(lhat_type_conforms(not_found, io), "NotFound is an IOError");
        LHAT_CHECK(lhat_type_conforms(not_found, simple(&t, LHAT_TYPE_ERROR)),
                   "and an error^");
        LHAT_CHECK(lhat_type_conforms(io, simple(&t, LHAT_TYPE_ERROR)),
                   "so is the set");
        LHAT_CHECK(!lhat_type_conforms(io, not_found),
                   "but the set is not the one kind");
        LHAT_CHECK(!lhat_type_conforms(simple(&t, LHAT_TYPE_ERROR), io),
                   "and error^ is not the set");
    }

    LHAT_TEST("identity is the declaration, not the shape");
    {
        LHAT_CHECK(!lhat_type_conforms(user_not_found, io),
                   "UserError.NotFound is not an IOError");
        LHAT_CHECK(!lhat_type_conforms(user_not_found, not_found),
                   "nor IOError.NotFound, despite the name");
        LHAT_CHECK(lhat_type_disjoint(user_not_found, not_found),
                   "they are separate");
    }

    // 2.6: this is what makes T|error^ always tellable apart, which is the
    // whole reason a kind is a declared type rather than a table.
    LHAT_TEST("an error is separate from everything that is not one");
    {
        LHAT_CHECK(lhat_type_disjoint(not_found, simple(&t, LHAT_TYPE_NUMBER)),
                   "not a number");
        LHAT_CHECK(lhat_type_disjoint(not_found,
                                      table1(&t, "kind",
                                             simple(&t, LHAT_TYPE_STRING))),
                   "not a table that looks like one");
        LHAT_CHECK(!lhat_type_disjoint(not_found, simple(&t, LHAT_TYPE_ERROR)),
                   "error^ overlaps every kind");
        LHAT_CHECK(lhat_type_disjoint(not_found, denied),
                   "two kinds of one set are separate");
    }

    // 7 章 of 04: exhaustiveness is union narrowing, so it needs nothing more
    // than the relations above.
    LHAT_TEST("a signature's error set is an ordinary union");
    {
        LhatType *result = lhat_type_union(&t.arena, simple(&t, LHAT_TYPE_STRING),
                                           io);
        LHAT_CHECK(lhat_type_conforms(not_found, result),
                   "a kind fits string^|IOError");
        LHAT_CHECK(!lhat_type_conforms(result, simple(&t, LHAT_TYPE_STRING)),
                   "and the union does not fit string^ unhandled");
        LHAT_CHECK(!lhat_type_conforms(user_not_found, result),
                   "a kind from another declaration does not fit");
    }

    // 2.5 with 6.1: narrowing is what makes a declared field visible.
    LHAT_TEST("a kind may declare fields");
    {
        LhatType *parse = lhat_type_error_set(&t.arena, "ParseError", 10);
        LhatType *syntax = lhat_type_error_kind(&t.arena, parse, "Syntax", 6);
        lhat_type_add_member(&t.arena, syntax, "line", 4,
                             simple(&t, LHAT_TYPE_NUMBER));
        LHAT_CHECK(syntax->v.error.fields != NULL, "the field is recorded");
        LHAT_CHECK_EQ_INT(syntax->v.error.fields->type->kind, LHAT_TYPE_NUMBER);
        LHAT_CHECK(lhat_type_disjoint(syntax, not_found),
                   "still separate from another declaration's kind");
    }

    types_dispose(&t);
}

int main(void)
{
    test_primitives();
    test_structures();
    test_composites();
    test_functions();
    test_errors();
    return lhat_test_report("test_type");
}
