// The semantic result, not the diagnostic policy, determines generated code.
#include "fixture.h"
#include "code.h"

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

int main(void)
{
    test_policy_parity();
    test_nominal_type_lowering();
    return lhat_test_report("test_pipeline");
}
