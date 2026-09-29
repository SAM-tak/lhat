// The semantic result, not the diagnostic policy, determines generated code.
#include "fixture.h"
#include "code.h"
#include "lhat/port.h"
#include <stdlib.h>

static size_t allocation_count;
static size_t fail_allocation;
static size_t live_allocations;

static bool refuse_allocation(void)
{
    return ++allocation_count == fail_allocation;
}

static void *test_alloc(void *context, size_t size)
{
    (void)context;
    if (refuse_allocation()) return NULL;
    void *p = malloc(size);
    if (p != NULL) live_allocations++;
    return p;
}

static void *test_calloc(void *context, size_t count, size_t size)
{
    (void)context;
    if (refuse_allocation()) return NULL;
    void *p = calloc(count, size);
    if (p != NULL) live_allocations++;
    return p;
}

static void *test_realloc(void *context, void *pointer, size_t size)
{
    (void)context;
    if (refuse_allocation()) return NULL;
    bool fresh = pointer == NULL;
    void *p = realloc(pointer, size);
    if (p != NULL && fresh) live_allocations++;
    return p;
}

static void test_free(void *context, void *pointer)
{
    (void)context;
    if (pointer != NULL) live_allocations--;
    free(pointer);
}

static void test_compile_allocation_failures(void)
{
    static const char *const sources[] = {
        "var^ x = 1\nlet^ f = f^n:number^ -> number^ {return^ x + n}\nreturn^ f(2)\n",
        "errordef^ E { Bad }\nlet^ f = f^ {return^ error^E.Bad{}}\n"
        "do^{ try^ f() catch^ E.Bad: return^ 42 }\n",
        "let^ D = def^{self^{x = 42}, m = f^self^ {return^ self^.x}}\nreturn^ D.new().m()\n",
        "enum^ E { V = 42 }\nrepeat^ 2 { if^ false^ {next^} break^ }\nreturn^ E.V.value\n",
        "let^ f = f^t:t^{x:number^} -> number^ {return^ t.x}\nreturn^ f({x = 42})\n",
        "let^ gen = p^ {yield^ 42}\nlet^ c = gen()\nreturn^ c.start()\n",
    };
    for (size_t s = 0; s < sizeof sources / sizeof *sources; s++) {
        LHAT_TEST("compiler allocation failures report OOM and release partial output");
        Unit u;
        check_text(&u, sources[s]);
        CHECK_CLEAN(&u);
        size_t baseline = live_allocations;
        allocation_count = 0;
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status, LHAT_COMPILE_OK);
        size_t count = allocation_count;
        lhat_proto_free(proto);
        for (size_t n = 1; n <= count; n++) {
            allocation_count = 0;
            fail_allocation = n;
            proto = NULL;
            LhatCompileResult result = lhat_compile(&u.checked, &u.lexer, &proto);
            fail_allocation = 0;
            LHAT_CHECK(result.status == LHAT_COMPILE_OUT_OF_MEMORY || result.status == LHAT_COMPILE_OK,
                       "source %zu allocation %zu returned status %d", s, n, result.status);
            if (result.status != LHAT_COMPILE_OK) {
                LHAT_CHECK(proto == NULL, "failed compile publishes no proto");
            } else {
                // Optional fast paths may fall back after a failed allocation,
                // but successful compilation must still mean correct output.
                LhatMachine *machine = lhat_machine_new();
                LhatRunResult ran = lhat_run(machine, proto);
                LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
                LHAT_CHECK(lhat_is_integer(ran.value) &&
                           lhat_as_integer(ran.value) == (s == 0 ? 3 : 42),
                           "allocation %zu may not silently change output", n);
                lhat_machine_dispose(machine);
            }
            lhat_proto_free(proto);
            LHAT_CHECK_EQ_INT(live_allocations, baseline);
        }
        unit_dispose(&u);
    }

    LHAT_TEST("local storage limit remains a complexity error rather than OOM");
    char source[8192];
    size_t used = 0;
    for (int i = 0; i < 210; i++) {
        used += (size_t)sprintf(source + used, "var^ n%d = 1\n", i);
    }
    Unit u;
    check_text(&u, source);
    CHECK_CLEAN(&u);
    LhatProto *proto = NULL;
    LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                      LHAT_COMPILE_TOO_COMPLEX);
    lhat_proto_free(proto);
    unit_dispose(&u);

    LHAT_TEST("a full constant pool rejects additions before allocating but reuses existing entries");
    LhatChunk chunk;
    lhat_chunk_init(&chunk);
    for (size_t i = 0; i <= 0xFFFF; i++) {
        LHAT_CHECK_EQ_INT(lhat_chunk_constant_raw(&chunk, lhat_integer((int64_t)i)), i);
    }
    allocation_count = 0;
    fail_allocation = 1;
    LHAT_CHECK(lhat_chunk_constant_raw(&chunk, lhat_nil()) == SIZE_MAX, "pool is full");
    LHAT_CHECK(lhat_chunk_string(&chunk, "new", 3) == SIZE_MAX, "new string exceeds pool limit");
    LHAT_CHECK_EQ_INT(lhat_chunk_constant(&chunk, lhat_integer(42)), 42);
    LHAT_CHECK_EQ_INT(allocation_count, 0);
    fail_allocation = 0;
    lhat_chunk_dispose(&chunk);
}

typedef struct {
    const LhatNode *nodes[1024];
    size_t count;
} Nodes;

static void collect(void *context, const char *field, bool in_list,
                    const LhatNode *node)
{
    (void)field;
    (void)in_list;
    Nodes *out = context;
    if (out->count == sizeof out->nodes / sizeof *out->nodes) {
        LHAT_CHECK(false, "test tree exceeds collection capacity");
        return;
    }
    out->nodes[out->count++] = node;
    lhat_node_visit_children(node, collect, out);
}

static void test_policy_parity(void)
{
    static const char *const sources[] = {
        "let^ f = f^ x { x + 1 }\nreturn^ f(2)\n",
        "let^ f = f^ x { 1 }\nreturn^ 1\n",
        "let^ e = {}\nlet^ value = e[0] ?? 0\n",
        "var^ t:t^{x:number^}|nil^ = nil^\nlet^ x = t.x\n",
        "var^ t:t^{x:number^}|nil^ = nil^\nlet^ x = t?.x ?? 0\n",
        "let^ use = f^ fn:(f^number^ -> number^;) { fn(1) }\n"
        "return^ use(f^ x {x + 1})\n",
        "let^ D = def^{self^{}, m = f^self^, x:number^ {x + 1},"
        " overload^ m = f^self^, x:string^ {2}}\nreturn^ D.new().m('a')\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST(sources[i]);
        Unit strict, relaxed;
        check_text(&strict, sources[i]);
        check_relaxed_text(&relaxed, sources[i]);
        LHAT_CHECK_EQ_INT(syntax_errors(&strict), 0);
        LHAT_CHECK_EQ_INT(syntax_errors(&relaxed), 0);
        LHAT_CHECK_EQ_INT(strict.checked.diagnostic_count, relaxed.checked.diagnostic_count);
        for (size_t j = 0; j < strict.checked.diagnostic_count &&
                           j < relaxed.checked.diagnostic_count; j++) {
            const LhatCheckDiagnostic *a = &strict.checked.diagnostics[j];
            const LhatCheckDiagnostic *b = &relaxed.checked.diagnostics[j];
            LHAT_CHECK_EQ_INT(a->code, b->code);
            LHAT_CHECK_EQ_INT(a->offset, b->offset);
            LHAT_CHECK_EQ_BOOL(a->relaxed_ok, b->relaxed_ok);
        }
        Nodes a = {0}, b = {0};
        collect(&a, NULL, false, strict.parsed.root);
        collect(&b, NULL, false, relaxed.parsed.root);
        LHAT_CHECK_EQ_INT(a.count, b.count);
        for (size_t j = 0; j < a.count && j < b.count; j++) {
            LHAT_CHECK(lhat_type_equal(a.nodes[j]->checked_type, b.nodes[j]->checked_type),
                       "type at offset %u differs by policy", a.nodes[j]->offset);
            LHAT_CHECK_EQ_INT(a.nodes[j]->checked_arm, b.nodes[j]->checked_arm);
        }
        if (lhat_check_error_count(&relaxed.checked) == 0) {
            LhatProto *p = NULL, *q = NULL;
            LHAT_CHECK_EQ_INT(lhat_compile(&strict.checked, &strict.lexer, &p).status, LHAT_COMPILE_OK);
            LHAT_CHECK_EQ_INT(lhat_compile(&relaxed.checked, &relaxed.lexer, &q).status, LHAT_COMPILE_OK);
            if (p != NULL && q != NULL) {
                LHAT_CHECK_EQ_INT(p->chunk.count, q->chunk.count);
                if (p->chunk.count == q->chunk.count) {
                    LHAT_CHECK(memcmp(p->chunk.code, q->chunk.code,
                                     p->chunk.count * sizeof *p->chunk.code) == 0,
                               "instruction selection differs by policy");
                }
            }
            lhat_proto_free(p);
            lhat_proto_free(q);
        }
        unit_dispose(&strict);
        unit_dispose(&relaxed);
    }
}

static void test_nominal_type_lowering(void)
{
    Run r;
    LHAT_TEST("nested error kinds retain their declaration identity");
    run_text(&r,
        "errordef^ E {A, B}\n"
        "let^ a = {value = error^ E.A}\n"
        "if^ a fits^ t^{value:E.B} {return^ 0}\n"
        "if^ a fits^ t^{value:E.A} {return^ 1}\nreturn^ 0\n");
    CHECK_INTEGER(&r, 1);
    run_dispose(&r);
}

static void test_binding_identity(void)
{
    LHAT_TEST("code generation follows resolved declarations, not use spellings");
    Unit u;
    check_text(&u,
        "var^ y = 100\nvar^ x = 40\n"
        "var^ bump = p^ {x := x + 2}\n"
        "bump()\nreturn^ x\n");
    CHECK_CLEAN(&u);
    Nodes nodes = {0};
    collect(&nodes, NULL, false, u.parsed.root);
    const LhatNode *x = u.parsed.root->v.list.items->next->v.binding.targets;
    const LhatNode *y = u.parsed.root->v.list.items->v.binding.targets;
    size_t uses = 0;
    for (size_t i = 0; i < nodes.count; i++) {
        LhatNode *node = (LhatNode *)nodes.nodes[i];
        if (node != x && node->checked_binding == x && node->kind == LHAT_NODE_IDENT) {
            // An emitter must consume the semantic reference even when the
            // display spelling changes after checking. This covers writes,
            // captures and the read-in-place operand optimization together.
            node->v.name = y->v.name;
            uses++;
        }
    }
    LHAT_CHECK_EQ_INT(uses, 3);
    LhatProto *proto = NULL;
    LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status, LHAT_COMPILE_OK);
    if (proto != NULL) {
        LhatMachine *machine = lhat_machine_new();
        LhatRunResult ran = lhat_run(machine, proto);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
        LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
        lhat_machine_dispose(machine);
        lhat_proto_free(proto);
    }
    unit_dispose(&u);

    LHAT_TEST("a missing resolved storage location is not rebound by spelling");
    check_text(&u, "var^ x = 1\nreturn^ x\n");
    LhatNode *use = u.parsed.root->v.list.items->next->v.jump.value;
    use->checked_binding = u.parsed.root->v.list.items->next;
    proto = NULL;
    LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                      LHAT_COMPILE_UNDEFINED);
    lhat_proto_free(proto);
    unit_dispose(&u);
}

static void test_narrowed_member_target_identity(void)
{
    LHAT_TEST("a narrowed member still resolves its parameter target in nested else arms");
    const char *source =
        "let^route = p^req:Request {\n"
        " if^req.user = nil^ {return^0\n"
        " el^req.body.total <= 0: return^-1\n"
        " el^: return^charge(req.user, req.body.total)}\n}\n"
        "let^Body = def^{self^{total = 40}}\n"
        "let^Request = def^{self^{user:string^|nil^ = \"ok\", body:Body = {total=40}}}\n"
        "let^charge = f^name:string^, amount:number^ {return^name.length + amount}\n"
        "return^route(Request.new())\n";
    for (int strict = 0; strict < 2; strict++) {
        Run r;
        if (strict) run_checked_text(&r, source);
        else run_text(&r, source);
        LHAT_CHECK_EQ_INT(lhat_check_error_count(&r.checked), 0);
        CHECK_INTEGER(&r, 42);
        Nodes nodes = {0};
        collect(&nodes, NULL, false, r.parsed.root);
        for (size_t j = 0; j < nodes.count; j++) {
            const LhatNode *node = nodes.nodes[j];
            const char *name = NULL;
            size_t length = 0;
            if (node->kind == LHAT_NODE_IDENT &&
                lhat_node_name(node, r.lexer.source->text, r.lexer.strings, &name, &length) &&
                length == 3 && memcmp(name, "req", 3) == 0) {
                LHAT_CHECK(node->checked_binding != NULL,
                           "every req occurrence has a declaration identity");
            }
        }
        run_dispose(&r);
    }
}

static void test_focus_and_catch_identity(void)
{
    static const char *const sources[] = {
        // Both counted-loop focuses, including a stacked reach captured by a body.
        "for^ 40 to^ 40 {for^ 2 to^ 2 {"
        "let^ read = f^ {return^ it^^ + it^}\nreturn^ read()}}\n",
        // A counted focus remains distinct from a named walking target.
        "for^ 40 to^ 40 {for^ n in^ {2} {return^ it^ + n}}\n",
        // Narrowing must preserve the catch binding's identity.
        "errordef^ E {A {n:number^}, B}\n"
        "let^ fail = f^ {return^ error^ E.A{n := 42}}\n"
        "return^ fail() catch^ if^ it^ fits^ E.A: it^.n el^: 0 ;\n",
        // Catch arms introduce an identity separate from the enclosing focus.
        "errordef^ E {A}\nlet^ fail = f^ {return^ error^ E.A{}}\n"
        "for^ 42 to^ 42 {do^{var^ ignored = try^ fail()\ncatch^:\n"
        "let^ read = f^ {return^ it^^}\nreturn^ read()}}\n",
        // An expression catch's error remains available to an escaping closure.
        "errordef^ E {A {n:number^}}\n"
        "let^ fail = f^ {return^ error^ E.A{n := 42}}\n"
        "let^ read = fail() catch^ (f^ {return^ it^.n})\nreturn^ read()\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST(sources[i]);
        Unit u;
        check_text(&u, sources[i]);
        CHECK_CLEAN(&u);
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        size_t uses = 0;
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            const char *name = NULL;
            size_t length = 0;
            if (node->kind != LHAT_NODE_HAT_IDENT ||
                !lhat_node_name(node, u.lexer.source->text, u.lexer.strings,
                                &name, &length) ||
                length != 3 || memcmp(name, "it^", 3) != 0) continue;
            LHAT_CHECK(node->checked_binding != NULL, "it^ has a resolved declaration");
            if (node->checked_binding == node) continue;
#if LHAT_WITH_RESOLUTIONS
            const LhatResolution *resolution = lhat_check_resolution_at(&u.checked, node->offset);
            LHAT_CHECK(resolution != NULL && resolution->has_definition,
                       "tooling retains the implicit binding's definition");
            if (resolution != NULL && node->checked_binding != NULL) {
                LHAT_CHECK_EQ_INT(resolution->definition, node->checked_binding->offset);
                LHAT_CHECK(lhat_type_equal(resolution->type, node->checked_type),
                           "tooling retains the narrowed reference type");
            }
#endif
            // The written reach no longer selects a binding after analysis.
            // Even a deliberately impossible count must use the recorded identity.
            node->v.name.hats = 7;
            uses++;
        }
        LHAT_CHECK(uses > 0, "the case exercises implicit references");
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_OK);
        if (proto != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
            if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
            lhat_proto_free(proto);
        }
        unit_dispose(&u);
    }
}

static void test_this_body_identity(void)
{
    static const char *const sources[] = {
        "let^ count = f^ n:number^ -> number^ {"
        "if^ n = 0 {return^ 0}\nreturn^ this^(n - 1) + 1}\nreturn^ count(42)\n",
        "let^ count = f^ n:number^ -> number^ {"
        "let^ middle = f^ -> number^ {let^ inner = f^ -> number^ {"
        "if^ n = 0 {return^ 0}\nreturn^ this^^^(n - 1) + 1}\n"
        "return^ inner()}\nreturn^ middle()}\nreturn^ count(42)\n",
        // Cache two different enclosing closures and reuse the outer one.
        "let^ outer = p^ -> (f^ -> number^;) {let^ a = this^\n"
        "let^ middle = p^ -> (f^ -> number^;) {let^ b = this^\n"
        "return^ f^ -> number^ {"
        "if^ this^^ is^ b and^ this^^^ is^ a and^ this^^^ is^ a {return^ 42}\n"
        "return^ 0}}\nreturn^ middle()}\nlet^ leaf = outer()\nreturn^ leaf()\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST(sources[i]);
        Unit u;
        check_text(&u, sources[i]);
        CHECK_CLEAN(&u);
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        size_t uses = 0;
        LhatNode *use = NULL;
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            const char *name = NULL;
            size_t length = 0;
            if (node->kind != LHAT_NODE_HAT_IDENT ||
                !lhat_node_name(node, u.lexer.source->text, u.lexer.strings,
                                &name, &length) ||
                length != 5 || memcmp(name, "this^", 5) != 0) continue;
            LHAT_CHECK(node->checked_this_body != NULL &&
                       node->checked_this_body->kind == LHAT_NODE_FUNC,
                       "this^ identifies a checked function literal");
            node->v.name.hats = 9;
            use = node;
            uses++;
        }
        LHAT_CHECK(uses > 0, "the case exercises body references");
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_OK);
        if (proto != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
            if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
            lhat_proto_free(proto);
        }
        if (use != NULL) {
            // A target absent from the frame chain cannot fall back to the spelling.
            use->checked_this_body = u.parsed.root;
            use->v.name.hats = 1;
            proto = NULL;
            LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                              LHAT_COMPILE_UNDEFINED);
            lhat_proto_free(proto);
        }
        unit_dispose(&u);
    }
}

static void test_this_body_repl_composition(void)
{
    LHAT_TEST("REPL composition preserves this^'s source body identity");
    TestSession *session = test_session_new();
    Run one, two;
    compile_next_text(&one, session,
        "let^ Base = def^{self^{}, count = f^self^, n:number^ -> number^ {"
        "if^ n = 0 {return^ 0}\nreturn^ this^(self^, n - 1) + 1}}\n");
    compile_next_text(&two, session,
        "let^ Derived = Base .. def^{self^{}}\nreturn^ Derived.new().count(42)\n");
    LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
    if (one.compiled == LHAT_COMPILE_OK && two.compiled == LHAT_COMPILE_OK) {
        LhatMachine *machine = lhat_machine_new();
        lhat_run(machine, one.proto);
        LhatRunResult result = lhat_run(machine, two.proto);
        LHAT_CHECK_EQ_INT(result.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_integer(result.value), "integer result");
        if (lhat_is_integer(result.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(result.value), 42);
        lhat_machine_dispose(machine);
    }
    test_session_dispose(session);
    compiled_dispose(&two);
    compiled_dispose(&one);
}

static void test_variadic_binding_identity(void)
{
    static const char *const sources[] = {
        "let^ decoy = {100}\n"
        "let^ outer = f^ ...:number^ -> number^ {"
        "let^ inner = f^ ...:number^ -> number^ {return^ (...[0] ?? 0)}\n"
        "return^ (...[0] ?? 0) + inner(2)}\nreturn^ outer(40)\n",
        "let^ decoy = {100}\n"
        "let^ make = f^ ...:number^ -> (f^ -> number^;) {"
        "return^ f^ -> number^ {return^ (...[0] ?? 0)}}\n"
        "let^ read = make(42)\nreturn^ read()\n",
        "let^ decoy = {100}\n"
        "let^ sum = f^ ...:number^ -> number^ {var^ total = 0\n"
        "for^ k, n in^ ... {total := total + n}\nreturn^ total}\n"
        "let^ forward = f^ ...:number^ -> number^ {return^ sum(2, ...)}\n"
        "return^ forward(40)\n",
        // The script collector is a binding too, captured by a non-variadic body.
        "let^ decoy = {100}\nlet^ read = f^ -> number^ {let^ n = ...[0]\n"
        "if^ n fits^ number^ {return^ n}\nreturn^ 0}\nreturn^ read()\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST(sources[i]);
        Unit u;
        check_text(&u, sources[i]);
        CHECK_CLEAN(&u);
        const LhatNode *decoy = u.parsed.root->v.list.items->v.binding.targets;
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        size_t uses = 0;
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            const char *name = NULL;
            size_t length = 0;
            if (node->kind != LHAT_NODE_HAT_IDENT ||
                !lhat_node_name(node, u.lexer.source->text, u.lexer.strings,
                                &name, &length) ||
                length != 3 || memcmp(name, "...", 3) != 0) continue;
            LHAT_CHECK(node->checked_binding != NULL, "collector has a declaration identity");
            if (node->checked_binding != NULL) {
                LHAT_CHECK(node->checked_binding == u.parsed.root ||
                           (node->checked_binding->kind == LHAT_NODE_PARAM &&
                            node->checked_binding->v.param.variadic),
                           "collector belongs to the script or a variadic parameter");
            }
            // All collectors now display the same competing lexical name.
            // Neither reads, captures, nor spread emission may rebind them.
            node->v.name = decoy->v.name;
            uses++;
        }
        LHAT_CHECK(uses > 0, "the case exercises collector references");
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_OK);
        if (proto != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatValue argument = lhat_integer(42);
            LhatRunResult ran = lhat_run_arguments(machine, proto, &argument, 1);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
            if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
            lhat_proto_free(proto);
        }
        unit_dispose(&u);
    }
}

static void test_def_binding_identity(void)
{
    static const char *const sources[] = {
        "let^ Outer = def^{self^{}, tag = 40, read = f^self^ -> number^ {"
        "let^ Inner = def^{self^{}, tag = 2, read = f^self^ -> number^ {"
        "return^ def^^.tag + def^.tag}}\nreturn^ Inner.new().read()}}\n"
        "let^ Derived = Outer .. def^{self^{}}\nreturn^ Derived.new().read()\n",
        // Source identities from both operands map to the composed table.
        "let^ A = def^{self^{}, ownerA = f^self^ -> any^ {return^ def^}}\n"
        "let^ B = def^{self^{}, ownerB = f^self^ -> any^ {return^ def^}}\n"
        "let^ D = A .. B\nlet^ d = D.new()\n"
        "if^ d.ownerA() is^ D and^ d.ownerB() is^ D and^ A.new().ownerA() is^ A "
        "{return^ 42}\nreturn^ 0\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST(sources[i]);
        Unit u;
        check_text(&u, sources[i]);
        CHECK_CLEAN(&u);
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        size_t uses = 0;
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            const char *name = NULL;
            size_t length = 0;
            if (node->kind != LHAT_NODE_HAT_IDENT ||
                !lhat_node_name(node, u.lexer.source->text, u.lexer.strings,
                                &name, &length) ||
                length != 4 || memcmp(name, "def^", 4) != 0) continue;
            LHAT_CHECK(node->checked_binding != NULL &&
                       node->checked_binding->kind == LHAT_NODE_DEF,
                       "def^ identifies its source definition");
            node->v.name.hats = 9;
            uses++;
        }
        LHAT_CHECK(uses > 0, "the case exercises definition references");
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_OK);
        if (proto != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
            if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
            lhat_proto_free(proto);
        }
        unit_dispose(&u);
    }

    LHAT_TEST("REPL composition maps prior def^ identities to the new table");
    TestSession *session = test_session_new();
    Run one, two;
    compile_next_text(&one, session,
        "let^ Base = def^{self^{}, owner = f^self^ -> any^ {return^ def^}}\n");
    compile_next_text(&two, session,
        "let^ Derived = Base .. def^{self^{}}\n"
        "if^ Derived.new().owner() is^ Derived {return^ 42}\nreturn^ 0\n");
    LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
    if (one.compiled == LHAT_COMPILE_OK && two.compiled == LHAT_COMPILE_OK) {
        LhatMachine *machine = lhat_machine_new();
        lhat_run(machine, one.proto);
        LhatRunResult result = lhat_run(machine, two.proto);
        LHAT_CHECK_EQ_INT(result.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_integer(result.value), "integer result");
        if (lhat_is_integer(result.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(result.value), 42);
        lhat_machine_dispose(machine);
    }
    test_session_dispose(session);
    compiled_dispose(&two);
    compiled_dispose(&one);
}

static void test_self_binding_identity(void)
{
    static const char *const sources[] = {
        "let^ Outer = def^{self^{x := 40}, read = f^self^ -> number^ {"
        "let^ Inner = def^{self^{y := 2}, read = f^self^ -> number^ {"
        "return^ self^^.x + self^.y}}\nreturn^ Inner.new().read()}}\n"
        "let^ Derived = Outer .. def^{self^{}}\nreturn^ Derived.new().read()\n",
        "let^ t = {n = 42, make = f^self^ -> (f^ -> number^;) {"
        "return^ f^ -> number^ {return^ self^.n}}}\n"
        "let^ read = t.make()\nreturn^ read()\n",
        "let^ Base = def^{self^{x := 0}, override^new = f^ n:number^ {self^{x = n}}}\n"
        "let^ Derived = Base .. def^{self^{y := 0}, override^new = f^ n:number^ {"
        "super^(n)\nself^{y = 2}}}\nlet^ d = Derived.new(40)\nreturn^ d.x + d.y\n",
        "let^ D = def^{self^{x := 40}, op^+ = f^ n:number^, self^ -> number^ {"
        "return^ n + self^.x}}\nreturn^ 2 + D.new()\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST(sources[i]);
        Unit u;
        check_text(&u, sources[i]);
        CHECK_CLEAN(&u);
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        size_t uses = 0;
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            if (node->checked_receiver != NULL) {
                LHAT_CHECK(node->checked_receiver->kind == LHAT_NODE_FUNC,
                           "implicit receiver identifies its introducing body");
                uses++;
            }
            const char *name = NULL;
            size_t length = 0;
            if (node->kind != LHAT_NODE_HAT_IDENT ||
                !lhat_node_name(node, u.lexer.source->text, u.lexer.strings,
                                &name, &length) ||
                length != 5 || memcmp(name, "self^", 5) != 0) continue;
            bool parameter = false;
            for (size_t k = 0; k < nodes.count; k++) {
                if (nodes.nodes[k]->kind == LHAT_NODE_PARAM &&
                    nodes.nodes[k]->v.param.name == node) parameter = true;
            }
            if (parameter) continue;
            LHAT_CHECK(node->checked_binding != NULL &&
                       node->checked_binding->kind == LHAT_NODE_FUNC,
                       "self^ identifies its introducing body");
            node->v.name.hats = 9;
            uses++;
        }
        LHAT_CHECK(uses > 0, "the case exercises receiver references");
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_OK);
        if (proto != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
            if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
            lhat_proto_free(proto);
        }
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            if (node->checked_receiver == NULL) continue;
            const LhatNode *receiver = node->checked_receiver;
            node->checked_receiver = u.parsed.root->v.list.items;
            proto = NULL;
            LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                              node->kind == LHAT_NODE_SELF_TABLE
                                  ? LHAT_COMPILE_UNSUPPORTED : LHAT_COMPILE_UNDEFINED);
            lhat_proto_free(proto);
            node->checked_receiver = receiver;
        }
        unit_dispose(&u);
    }
}

static void test_super_binding_identity(void)
{
    static const char *const sources[] = {
        "let^ decoy = f^ -> number^ {return^ 1000}\n"
        "let^ A = def^{self^{}, m = f^self^ -> number^ {return^ 40}}\n"
        "let^ D = A .. def^{self^{}, override^ m = f^self^ -> number^ {"
        "let^ read = f^ -> number^ {return^ super^()}\nreturn^ read() + 2}}\n"
        "return^ D.new().m()\n",
        "let^ decoy = f^ -> number^ {return^ 1000}\n"
        "let^ A = def^{self^{}, m = f^ -> number^ {return^ 40}}\n"
        "let^ D = A .. def^{self^{}, override^ m = f^ -> number^ {return^ super^() + 2}}\n"
        "return^ D.m()\n",
        "let^ decoy = f^ -> number^ {return^ 1000}\n"
        "let^ A = def^{self^{x := 0}, override^new = f^ n:number^ {self^{x = n}}}\n"
        "let^ B = A .. def^{self^{}, override^new = f^ n:number^ {super^(n + 1)}}\n"
        "let^ D = B .. def^{self^{}, override^new = f^ n:number^ {super^(n + 1)}}\n"
        "return^ D.new(40).x\n",
        "let^ decoy = f^ -> number^ {return^ 1000}\n"
        "let^ A = def^{self^{}, m = f^self^ -> number^ {return^ 20}}\n"
        "let^ D = A .. def^{self^{}, override^ m = f^self^ -> number^ {"
        "let^ Inner = A .. def^{self^{}, override^ m = f^self^ -> number^ {"
        "return^ super^() + 2}}\nreturn^ Inner.new().m() + super^()}}\n"
        "return^ D.new().m()\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST(sources[i]);
        Unit u;
        check_text(&u, sources[i]);
        CHECK_CLEAN(&u);
        const LhatNode *decoy = u.parsed.root->v.list.items->v.binding.targets;
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        size_t uses = 0, calls = 0;
        LhatNode *use = NULL;
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            if (node->checked_super_call) calls++;
            const char *name = NULL;
            size_t length = 0;
            if (node->kind != LHAT_NODE_HAT_IDENT ||
                !lhat_node_name(node, u.lexer.source->text, u.lexer.strings,
                                &name, &length) ||
                length != 6 || memcmp(name, "super^", 6) != 0) continue;
            LHAT_CHECK(node->checked_binding != NULL &&
                       node->checked_binding->v.entry.modifier == LHAT_DEF_OVERRIDE,
                       "super^ identifies the override's replaced-value binding");
            // Both value lookup and implicit receiver passing must survive a
            // display spelling which looks like an ordinary static function.
            node->v.name = decoy->v.name;
            use = node;
            uses++;
        }
        LHAT_CHECK(uses > 0 && calls > 0, "the case exercises direct super calls");
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_OK);
        if (proto != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
            if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
            lhat_proto_free(proto);
        }
        if (use != NULL) {
            use->checked_binding = u.parsed.root->v.list.items;
            proto = NULL;
            LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                              LHAT_COMPILE_UNDEFINED);
            lhat_proto_free(proto);
        }
        unit_dispose(&u);
    }
}

static void test_super_repl_composition(void)
{
    LHAT_TEST("REPL composition preserves a prior override's super binding");
    TestSession *session = test_session_new();
    Run one, two;
    compile_next_text(&one, session,
        "let^ Base = def^{self^{}, m = f^self^ -> number^ {return^ 40}}\n"
        "let^ Middle = Base .. def^{self^{}, override^ m = f^self^ -> number^ {"
        "return^ super^() + 2}}\n");
    compile_next_text(&two, session,
        "let^ Derived = Middle .. def^{self^{}}\nreturn^ Derived.new().m()\n");
    LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
    if (one.compiled == LHAT_COMPILE_OK && two.compiled == LHAT_COMPILE_OK) {
        LhatMachine *machine = lhat_machine_new();
        lhat_run(machine, one.proto);
        LhatRunResult result = lhat_run(machine, two.proto);
        LHAT_CHECK_EQ_INT(result.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_integer(result.value), "integer result");
        if (lhat_is_integer(result.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(result.value), 42);
        lhat_machine_dispose(machine);
    }
    test_session_dispose(session);
    compiled_dispose(&two);
    compiled_dispose(&one);
}

static void test_enum_binding_identity(void)
{
    LHAT_TEST("enum reads and captures use the resolved declaration");
    Unit u;
    check_text(&u,
        "enum^ Wrong {V = 100}\nenum^ E {V = 42}\n"
        "let^ read = f^ -> number^ {return^ E.V.value}\nreturn^ read()\n");
    CHECK_CLEAN(&u);
    const LhatNode *wrong = u.parsed.root->v.list.items->v.named.name;
    const LhatNode *wanted = u.parsed.root->v.list.items->next->v.named.name;
    Nodes nodes = {0};
    collect(&nodes, NULL, false, u.parsed.root);
    size_t uses = 0;
    for (size_t i = 0; i < nodes.count; i++) {
        LhatNode *node = (LhatNode *)nodes.nodes[i];
        if (node != wanted && node->checked_binding == wanted) {
            node->v.name = wrong->v.name;
            uses++;
        }
    }
    LHAT_CHECK_EQ_INT(uses, 1);
    LhatProto *proto = NULL;
    LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status, LHAT_COMPILE_OK);
    if (proto != NULL) {
        LhatMachine *machine = lhat_machine_new();
        LhatRunResult ran = lhat_run(machine, proto);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
        if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
        lhat_machine_dispose(machine);
        lhat_proto_free(proto);
    }
    unit_dispose(&u);
}

static void test_storage_binding_identity(void)
{
    static const char *const bodies[] = {
        "let^ pair = f^ -> (number^, number^) {return^ 40, 2}\n"
        "let^ a, b = pair()\nreturn^ a + b\n",
        "var^ total = 0\nfor^ n from^ 1 to^ 6 {total := total + n}\nreturn^ total * 2\n",
        "var^ root.a = 40\ndo^{var^ root.b = 2}\nreturn^ root.a + root.b\n",
        "var^ a = 2\nvar^ b = 40\na, b := b, a\nreturn^ a + b\n",
        "let^ state = {n := 0}\n"
        "let^ Resource = def^{self^{}, dispose = p^self^ {state.n := state.n + 2}}\n"
        "with^ resource = Resource.new() {state.n := 40}\nreturn^ state.n\n",
        "let^ read = f^ -> number^ {return^ E.V.value}\nenum^ E {V = 42}\nreturn^ read()\n",
        "let^ _^ = 1\nlet^ _^ = 2\nreturn^ 42\n",
    };
    for (size_t i = 0; i < sizeof bodies / sizeof *bodies; i++) {
        LHAT_TEST(bodies[i]);
        char source[2048];
        snprintf(source, sizeof source, "let^ decoy = 1000\n%s", bodies[i]);
        Unit u;
        check_text(&u, source);
        CHECK_CLEAN(&u);
        const LhatNode *decoy = u.parsed.root->v.list.items->v.binding.targets;
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            // Distinct declarations now have the same display name, while
            // references keep the checker's identities. Every storage choice
            // (including initialization and cleanup) must follow those IDs.
            if (node->kind == LHAT_NODE_IDENT && node->checked_binding != NULL) {
                node->v.name = decoy->v.name;
            }
        }
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_OK);
        if (proto != NULL) {
            LhatMachine *machine = lhat_machine_new();
            LhatRunResult ran = lhat_run(machine, proto);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
            if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
            lhat_machine_dispose(machine);
            lhat_proto_free(proto);
        }
        unit_dispose(&u);
    }
}

static void test_session_storage_identity(void)
{
    LHAT_TEST("REPL slot retention and redefinition use IDs despite colliding display names");
    static const char *const sources[] = {
        "let^ decoy = 1000\nvar^ x = 38\nvar^ y = 2\n",
        "var^ x = x + 2\n",
        "return^ x + y\n",
    };
    TestSession *session = test_session_new();
    Unit units[3];
    LhatProto *protos[3] = {0};
    for (size_t i = 0; i < 3; i++) {
        check_next_text(&units[i], session->checks, sources[i]);
        CHECK_CLEAN(&units[i]);
        if (i == 0) {
            const LhatNode *first = units[i].parsed.root->v.list.items;
            for (const LhatNode *s = first->next; s != NULL; s = s->next) {
                s->v.binding.targets->v.name = first->v.binding.targets->v.name;
            }
        }
        LHAT_CHECK_EQ_INT(lhat_compile_next(session->compiles, &units[i].checked,
                                            &units[i].lexer, &protos[i]).status,
                          LHAT_COMPILE_OK);
    }
    if (protos[0] != NULL && protos[1] != NULL && protos[2] != NULL) {
        LhatMachine *machine = lhat_machine_new();
        lhat_run(machine, protos[0]);
        lhat_run(machine, protos[1]);
        LhatRunResult ran = lhat_run(machine, protos[2]);
        LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
        LHAT_CHECK(lhat_is_integer(ran.value), "integer result");
        if (lhat_is_integer(ran.value)) LHAT_CHECK_EQ_INT(lhat_as_integer(ran.value), 42);
        lhat_machine_dispose(machine);
    }
    test_session_dispose(session);
    for (size_t i = 0; i < 3; i++) {
        lhat_proto_free(protos[i]);
        unit_dispose(&units[i]);
    }
}

static void test_session_allocation_rollback(void)
{
    LHAT_TEST("a failed REPL compile preserves retained names and error kinds for retry");
    size_t allocations = 1;
    for (size_t n = 1; n <= allocations; n++) {
        TestSession *session = test_session_new();
        Unit units[3];
        LhatProto *protos[3] = {0};
        check_next_text(&units[0], session->checks,
                        "errordef^ E { Bad }\nvar^ x = 41\n");
        CHECK_CLEAN(&units[0]);
        LHAT_CHECK_EQ_INT(lhat_compile_next(session->compiles, &units[0].checked,
                                            &units[0].lexer, &protos[0]).status, LHAT_COMPILE_OK);
        check_next_text(&units[1], session->checks,
                        "errordef^ F { Bad }\nvar^ y = 1\nlet^ e = error^E.Bad{}\n");
        CHECK_CLEAN(&units[1]);
        size_t baseline = live_allocations;
        allocation_count = 0;
        fail_allocation = n;
        LhatCompileResult result = lhat_compile_next(session->compiles, &units[1].checked,
                                                    &units[1].lexer, &protos[1]);
        fail_allocation = 0;
        if (result.status != LHAT_COMPILE_OK) {
            LHAT_CHECK_EQ_INT(result.status, LHAT_COMPILE_OUT_OF_MEMORY);
            LHAT_CHECK(protos[1] == NULL, "failed input publishes no proto");
            LHAT_CHECK_EQ_INT(live_allocations, baseline);
            allocation_count = 0;
            LHAT_CHECK_EQ_INT(lhat_compile_next(session->compiles, &units[1].checked,
                                                &units[1].lexer, &protos[1]).status, LHAT_COMPILE_OK);
        }
        allocations = allocation_count;
        check_next_text(&units[2], session->checks, "return^ x + y\n");
        CHECK_CLEAN(&units[2]);
        LHAT_CHECK_EQ_INT(lhat_compile_next(session->compiles, &units[2].checked,
                                            &units[2].lexer, &protos[2]).status, LHAT_COMPILE_OK);
        if (protos[0] != NULL && protos[1] != NULL && protos[2] != NULL) {
            LhatMachine *machine = lhat_machine_new();
            lhat_run(machine, protos[0]);
            lhat_run(machine, protos[1]);
            LhatRunResult ran = lhat_run(machine, protos[2]);
            LHAT_CHECK_EQ_INT(ran.status, LHAT_RUN_OK);
            LHAT_CHECK(lhat_is_integer(ran.value) && lhat_as_integer(ran.value) == 42,
                       "retry retains both inputs");
            lhat_machine_dispose(machine);
        }
        test_session_dispose(session);
        for (size_t i = 0; i < 3; i++) {
            lhat_proto_free(protos[i]);
            unit_dispose(&units[i]);
        }
    }
}

static void test_missing_binding_metadata(void)
{
    static const char *const sources[] = {
        "var^ x = 42\nreturn^ x\n",
        "var^ x = 41\nreturn^ x + 1\n",
        "var^ x = 42\nlet^ read = f^ -> number^ {return^ x}\nreturn^ read()\n",
        "var^ x = 42\ndo^{var^ x = 0\nreturn^ $x}\n",
        "var^ x = 40\ndo^{$x := 42}\nreturn^ x\n",
        "enum^ x {V = 42}\nreturn^ x.V.value\n",
    };
    for (size_t i = 0; i < sizeof sources / sizeof *sources; i++) {
        LHAT_TEST("missing metadata never falls back to names or scope specifiers");
        Unit u;
        check_text(&u, sources[i]);
        CHECK_CLEAN(&u);
        Nodes nodes = {0};
        collect(&nodes, NULL, false, u.parsed.root);
        bool removed = false;
        for (size_t j = 0; j < nodes.count; j++) {
            LhatNode *node = (LhatNode *)nodes.nodes[j];
            const char *name = NULL;
            size_t length = 0;
            if (node->checked_binding != NULL && node->checked_binding != node &&
                lhat_node_name(node, u.lexer.source->text, u.lexer.strings, &name, &length) &&
                length == 1 && name[0] == 'x') {
                node->checked_binding = NULL;
                removed = true;
                break;
            }
        }
        LHAT_CHECK(removed, "the reference metadata was removed");
        LhatProto *proto = NULL;
        LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                          LHAT_COMPILE_UNDEFINED);
        lhat_proto_free(proto);
        unit_dispose(&u);
    }

    LHAT_TEST("an invalid scope remains an analysis error after changing the written reach");
    Unit u;
    check_text(&u, "var^ x = 42\nreturn^ $^^x\n");
    LhatNode *use = u.parsed.root->v.list.items->next->v.jump.value;
    LHAT_CHECK(use->checked_scope_invalid, "analysis rejected the scope reach");
    use->v.scope.depth = 0;
    LhatProto *proto = NULL;
    LHAT_CHECK_EQ_INT(lhat_compile(&u.checked, &u.lexer, &proto).status,
                      LHAT_COMPILE_SCOPE_TOO_FAR);
    lhat_proto_free(proto);
    unit_dispose(&u);
}

static void test_error_declaration_identity(void)
{
    Run r;
    LHAT_TEST("same-spelled error declarations in nested scopes stay distinct");
    run_text(&r,
        "errordef^ E {K {value := 40}}\n"
        "var^ total = 0\n"
        "do^{\n"
        "  errordef^ E {K {value := 2}}\n"
        "  var^ inner = error^ E.K\n"
        "  total := inner.value\n"
        "}\n"
        "var^ outer = error^ E.K\nreturn^ total + outer.value\n");
    CHECK_INTEGER(&r, 42);
    LHAT_CHECK_EQ_INT(lhat_check_error_count(&r.checked), 0);
    run_dispose(&r);

    LHAT_TEST("error defaults retain the declaration's lexical binding");
    run_text(&r,
        "var^ x = 42\ndo^{errordef^ E {K {value := x}}\n"
        "do^{var^ x = 100\nvar^ e = error^ E.K\nreturn^ e.value}}\n");
    CHECK_INTEGER(&r, 42);
    LHAT_CHECK_EQ_INT(lhat_check_error_count(&r.checked), 0);
    run_dispose(&r);

    LHAT_TEST("REPL error defaults are read with their declaring source");
    TestSession *session = test_session_new();
    Run one, two;
    compile_next_text(&one, session, "errordef^ E {K {value := 42}}\n");
    compile_next_text(&two, session, "var^ e = error^ E.K\nreturn^ e.value\n");
    LHAT_CHECK_EQ_INT(one.compiled, LHAT_COMPILE_OK);
    LHAT_CHECK_EQ_INT(two.compiled, LHAT_COMPILE_OK);
    if (one.compiled == LHAT_COMPILE_OK && two.compiled == LHAT_COMPILE_OK) {
        LhatMachine *machine = lhat_machine_new();
        lhat_run(machine, one.proto);
        LhatRunResult result = lhat_run(machine, two.proto);
        LHAT_CHECK_EQ_INT(result.status, LHAT_RUN_OK);
        LHAT_CHECK_EQ_INT(lhat_as_integer(result.value), 42);
        lhat_machine_dispose(machine);
    }
    test_session_dispose(session);
    compiled_dispose(&two);
    compiled_dispose(&one);
}

static void test_definition_origins(void)
{
    Run r;
    LHAT_TEST("composition follows the shadowing definition, not the first spelling");
    run_text(&r,
        "var^ Base = def^{self^{x := 1}}\n"
        "do^{var^ Base = def^{self^{x := 40}}\n"
        "let^ Alias = Base\n"
        "var^ Derived = Alias .. def^{self^{y := 2}}\n"
        "var^ d = Derived.new()\nreturn^ d.x + d.y}\n");
    CHECK_INTEGER(&r, 42);
    LHAT_CHECK_EQ_INT(lhat_check_error_count(&r.checked), 0);
    run_dispose(&r);

    LHAT_TEST("a deferred composition can use a later resolved declaration");
    run_text(&r,
        "var^ make = f^ {\n"
        "  var^ Derived = Base .. def^{self^{y := 2}}\n"
        "  var^ d = Derived.new()\nreturn^ d.x + d.y}\n"
        "var^ Base = def^{self^{x := 40}}\nreturn^ make()\n");
    CHECK_INTEGER(&r, 42);
    LHAT_CHECK_EQ_INT(lhat_check_error_count(&r.checked), 0);
    run_dispose(&r);

    LHAT_TEST("knowing a call's result type does not prove a static composition origin");
    Unit u;
    check_text(&u,
        "let^ make = f^ {return^ def^{self^{x := 1}}}\n"
        "let^ Dynamic = make()\nlet^ Derived = Dynamic .. def^{self^{y := 2}}\n");
    const LhatNode *value = u.parsed.root->v.list.items->next->next->v.binding.values;
    LHAT_CHECK(value->checked_definition == NULL,
               "a runtime call must not be replaced by flattening its result type");
    unit_dispose(&u);
}

int main(void)
{
    LhatAllocator allocator = {test_alloc, test_calloc, test_realloc, test_free, NULL};
    if (!lhat_set_allocator(&allocator)) return 1;
    test_compile_allocation_failures();
    test_policy_parity();
    test_nominal_type_lowering();
    test_binding_identity();
    test_focus_and_catch_identity();
    test_narrowed_member_target_identity();
    test_this_body_identity();
    test_this_body_repl_composition();
    test_variadic_binding_identity();
    test_def_binding_identity();
    test_self_binding_identity();
    test_super_binding_identity();
    test_super_repl_composition();
    test_enum_binding_identity();
    test_storage_binding_identity();
    test_session_storage_identity();
    test_session_allocation_rollback();
    test_missing_binding_metadata();
    test_error_declaration_identity();
    test_definition_origins();
    return lhat_test_report("test_pipeline");
}
