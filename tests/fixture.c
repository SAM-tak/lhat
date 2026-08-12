// L^ (lhat) -- the shared fixtures' bodies. See fixture.h for what each one
// is for.

#include "fixture.h"

// ---------------------------------------------------------------------------
// Parse

void parse_text(Parse *p, const char *text)
{
    lhat_source_init_from_string(&p->source, "<test>", text, strlen(text));
    lhat_lexer_init(&p->lexer, &p->source);
    lhat_parse(&p->lexer, &p->result);
}

void parse_interactive_text(Parse *p, const char *text)
{
    lhat_source_init_from_string(&p->source, "<test>", text, strlen(text));
    lhat_lexer_init(&p->lexer, &p->source);
    lhat_parse_interactive(&p->lexer, &p->result);
}

void parse_dispose(Parse *p)
{
    lhat_parse_result_dispose(&p->result);
    lhat_lexer_dispose(&p->lexer);
    lhat_source_dispose(&p->source);
}

// ---------------------------------------------------------------------------
// Unit

static void unit_text(Unit *u, const char *text, bool interactive)
{
    lhat_source_init_from_string(&u->source, "<test>", text, strlen(text));
    lhat_lexer_init(&u->lexer, &u->source);
    if (interactive) {
        lhat_parse_interactive(&u->lexer, &u->parsed);
    } else {
        lhat_parse(&u->lexer, &u->parsed);
    }
}

void check_text(Unit *u, const char *text)
{
    unit_text(u, text, false);
    lhat_check(u->parsed.root, &u->lexer, true, &u->checked);
}

void check_relaxed_text(Unit *u, const char *text)
{
    unit_text(u, text, false);
    lhat_check(u->parsed.root, &u->lexer, false, &u->checked);
}

void check_next_text(Unit *u, LhatCheckSession *s, const char *text)
{
    unit_text(u, text, false);
    lhat_check_next(s, u->parsed.root, &u->lexer, true, &u->checked);
}

void check_asked_text(Unit *u, LhatCheckSession *s, const char *text)
{
    unit_text(u, text, true);
    lhat_check_next(s, u->parsed.root, &u->lexer, true, &u->checked);
}

void unit_dispose(Unit *u)
{
    lhat_check_result_dispose(&u->checked);
    lhat_parse_result_dispose(&u->parsed);
    lhat_lexer_dispose(&u->lexer);
    lhat_source_dispose(&u->source);
}

size_t syntax_errors(const Unit *u)
{
    return u->parsed.diagnostic_count + u->lexer.diagnostic_count;
}

bool has_error(const Unit *u, LhatCheckErrorCode code)
{
    for (size_t i = 0; i < u->checked.diagnostic_count; i++) {
        if (u->checked.diagnostics[i].code == code) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Run

static void run_source(Run *r, const char *text, bool interactive)
{
    lhat_source_init_from_string(&r->source, "<test>", text, strlen(text));
    lhat_lexer_init(&r->lexer, &r->source);
    if (interactive) {
        lhat_parse_interactive(&r->lexer, &r->parsed);
    } else {
        lhat_parse(&r->lexer, &r->parsed);
    }
    r->machine = NULL;
    memset(&r->ran, 0, sizeof r->ran);
}

void compile_text(Run *r, const char *text)
{
    run_source(r, text, false);
    r->compile_result = lhat_compile(r->parsed.root, &r->lexer, &r->proto);
    r->compiled = r->compile_result.status;
}

void compile_next_text(Run *r, LhatCompileSession *s, const char *text)
{
    run_source(r, text, false);
    r->compile_result =
        lhat_compile_next(s, r->parsed.root, &r->lexer, &r->proto);
    r->compiled = r->compile_result.status;
}

void compile_asked_text(Run *r, LhatCompileSession *s, const char *text)
{
    run_source(r, text, true);
    r->compile_result =
        lhat_compile_next(s, r->parsed.root, &r->lexer, &r->proto);
    r->compiled = r->compile_result.status;
}

void compiled_dispose(Run *r)
{
    lhat_proto_free(r->proto);
    lhat_parse_result_dispose(&r->parsed);
    lhat_lexer_dispose(&r->lexer);
    lhat_source_dispose(&r->source);
}

void run_text(Run *r, const char *text)
{
    compile_text(r, text);
    if (r->compiled != LHAT_COMPILE_OK) {
        return;
    }
    r->machine = lhat_machine_new();
    if (r->machine != NULL) {
        r->ran = lhat_run(r->machine, r->proto);
    }
}

void run_dispose(Run *r)
{
    lhat_machine_dispose(r->machine);
    r->machine = NULL;
    compiled_dispose(r);
}

// The result is disposed after compiling, not before: checked_type points
// into its arena, and the compiler reads it.
void run_checked_text(Run *r, const char *text)
{
    run_source(r, text, false);

    LhatCheckResult checked;
    lhat_check(r->parsed.root, &r->lexer, true, &checked);
    r->compile_result = lhat_compile(r->parsed.root, &r->lexer, &r->proto);
    r->compiled = r->compile_result.status;
    lhat_check_result_dispose(&checked);

    if (r->compiled != LHAT_COMPILE_OK) {
        return;
    }
    r->machine = lhat_machine_new();
    if (r->machine != NULL) {
        r->ran = lhat_run(r->machine, r->proto);
    }
}
