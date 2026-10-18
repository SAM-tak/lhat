// L^ (lhat) -- a prompt (lhat/prompt.h): 03 の 4 章's loop, less the
// reading and the printing, which are the host's.
//
// Section numbers refer to DesignDocuments/03-compilation-pipeline.md unless
// prefixed.

#include "lhat/prompt.h"

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "compile.h"
#include "parser.h"
#include "program_internal.h"

// Every input's pieces have to outlive the run -- a proto is what the
// machine runs, and names point into the lexer's source. The prompt keeps
// them all until it ends.
typedef struct {
    LhatSource source;
    LhatLexer lexer;
    LhatParseResult parsed;
    LhatProto *proto;
} Held;

struct LhatPrompt {
    LhatProgram *program;
    LhatMachine *machine;
    char *name;
    LhatCheckSession *checks;
    LhatCompileSession *compiles;

    Held *held;
    size_t held_count;
    size_t held_capacity;

    // What has been fed since the last whole input (02 の 3.1).
    char *input;
    size_t input_length;

    // What the last feed was told, entries a line apart.
    char *report;
    size_t report_length;
    size_t report_capacity;
};

// 05 の 8.7: what LhatRequire and LhatUnits carry for a file -- the registry
// import^ resolves against, 8.6's L^ members and 8.2's bound names for the
// checker, the error kinds and hostdata types fits^ names for the compiler.
// All of it is the program's, which is why it has to outlive the prompt.
static void install_checks(const LhatProgram *program,
                           LhatCheckSession *session)
{
    lhat_check_session_hosted(session, program->hosted, program->globals);
    lhat_check_session_bind(session,
                            (const char *const *)program->initial_names,
                            (const char *const *)program->initial_members,
                            program->initial_count);
}

static void install_compiles(const LhatProgram *program,
                             LhatCompileSession *session)
{
    lhat_compile_session_hosted(session, program->host_error_entries,
                                program->host_error_entry_count,
                                program->host_type_entries,
                                program->host_type_entry_count);
}

LhatPrompt *lhat_prompt_new(LhatProgram *program, LhatMachine *machine,
                            const char *name)
{
    LhatPrompt *prompt = (LhatPrompt *)calloc(1, sizeof *prompt);
    size_t length = name != NULL ? strlen(name) : 0;
    if (prompt == NULL || (prompt->name = (char *)malloc(length + 1)) == NULL ||
        (prompt->checks = lhat_check_session_new()) == NULL ||
        (prompt->compiles = lhat_compile_session_new()) == NULL) {
        lhat_prompt_dispose(prompt);
        return NULL;
    }
    memcpy(prompt->name, name != NULL ? name : "", length + 1);
    prompt->program = program;
    prompt->machine = machine;
    install_checks(program, prompt->checks);
    install_compiles(program, prompt->compiles);
    return prompt;
}

static void drop_held(Held *in)
{
    lhat_proto_free(in->proto);
    lhat_parse_result_dispose(&in->parsed);
    lhat_lexer_dispose(&in->lexer);
    lhat_source_dispose(&in->source);
}

void lhat_prompt_dispose(LhatPrompt *prompt)
{
    if (prompt == NULL) {
        return;
    }
    for (size_t i = 0; i < prompt->held_count; i++) {
        drop_held(&prompt->held[i]);
    }
    free(prompt->held);
    free(prompt->input);
    free(prompt->report);
    if (prompt->compiles != NULL) {
        lhat_compile_session_dispose(prompt->compiles);
    }
    if (prompt->checks != NULL) {
        lhat_check_session_dispose(prompt->checks);
    }
    free(prompt->name);
    free(prompt);
}

bool lhat_prompt_drop(LhatPrompt *prompt)
{
    bool carried = prompt->input_length > 0;
    free(prompt->input);
    prompt->input = NULL;
    prompt->input_length = 0;
    return carried;
}

size_t lhat_prompt_report(const LhatPrompt *prompt, char *out,
                          size_t capacity)
{
    size_t length = prompt->report_length;
    if (out != NULL && capacity > 0) {
        size_t take = length < capacity ? length : capacity - 1;
        if (take > 0) {
            memcpy(out, prompt->report, take);
        }
        out[take] = '\0';
    }
    return length;
}

// Room at the end of the report for `needed` more bytes and their NUL, a
// line after what is there. NULL with no room, which leaves it as it was.
static char *report_room(LhatPrompt *prompt, size_t needed)
{
    size_t gap = prompt->report_length > 0 ? 1 : 0;
    size_t want = prompt->report_length + gap + needed + 1;
    if (want > prompt->report_capacity) {
        size_t grown = prompt->report_capacity ? prompt->report_capacity : 256;
        while (grown < want) {
            grown *= 2;
        }
        char *bigger = (char *)realloc(prompt->report, grown);
        if (bigger == NULL) {
            return NULL;
        }
        prompt->report = bigger;
        prompt->report_capacity = grown;
    }
    if (gap) {
        prompt->report[prompt->report_length++] = '\n';
    }
    return prompt->report + prompt->report_length;
}

// One diagnostic, with the line it points into (lhat_report_write's rich
// form -- a prompt always has the source to hand).
static void tell(LhatPrompt *prompt, const Held *in, LhatReportKind kind,
                 const char *message, uint32_t offset, uint32_t line,
                 uint32_t column, uint32_t length)
{
    LhatReport report;
    report.kind = kind;
    report.message = message != NULL ? message : "";
    report.offset = offset;
    report.line = line;
    report.column = column;
    report.length = length;
    size_t needed = lhat_report_write(prompt->program, &report, &in->source,
                                      prompt->name, true, NULL, 0);
    char *room = report_room(prompt, needed);
    if (room != NULL) {
        prompt->report_length += lhat_report_write(
            prompt->program, &report, &in->source, prompt->name, true, room,
            needed + 1);
    }
}

// The parser's and the checker's messages can name a token or a name, which
// no literal carries, so each is written out before it is told.
#define TELL_WRITTEN(prompt, in, kind, write, d, offset, length)            \
    do {                                                                    \
        size_t needed_ = write((prompt)->program, (d), NULL, 0);            \
        char *text_ = (char *)malloc(needed_ + 1);                          \
        if (text_ != NULL) {                                                \
            write((prompt)->program, (d), text_, needed_ + 1);              \
        }                                                                   \
        tell((prompt), (in), (kind), text_, (offset), (d)->line,            \
             (d)->column, (length));                                        \
        free(text_);                                                        \
    } while (0)

// Reads, checks and compiles `in`, telling what each stage said. False when
// any of them refused it.
static bool accept(LhatPrompt *prompt, Held *in)
{
    bool strict = prompt->program->strict;
    bool refused = in->lexer.diagnostic_count > 0;
    for (size_t i = 0; i < in->lexer.diagnostic_count; i++) {
        const LhatDiagnostic *d = &in->lexer.diagnostics[i];
        tell(prompt, in, LHAT_REPORT_ERROR,
             lhat_program_text(prompt->program, lhat_lexer_error_id(d->code),
                               lhat_lexer_error_message(d->code)),
             d->offset, d->line, d->column, 0);
    }
    for (size_t i = 0; i < in->parsed.diagnostic_count; i++) {
        const LhatParseDiagnostic *d = &in->parsed.diagnostics[i];
        TELL_WRITTEN(prompt, in, LHAT_REPORT_ERROR, lhat_parse_message_write,
                     d, d->offset, d->length);
        refused = true;
    }
    if (refused) {
        return false;
    }

    LhatCheckResult checked = {0};
    lhat_check_next(prompt->checks, in->parsed.root, &in->lexer, strict,
                    &checked);
    for (size_t i = 0; i < checked.diagnostic_count; i++) {
        const LhatCheckDiagnostic *d = &checked.diagnostics[i];
        // Relaxed tells about these and runs the input anyway.
        bool warning = !strict && d->relaxed_ok;
        TELL_WRITTEN(prompt, in,
                     warning ? LHAT_REPORT_WARNING : LHAT_REPORT_ERROR,
                     lhat_check_message_write, d, d->offset,
                     d->highlight_length != 0 ? d->highlight_length
                                              : d->name_length);
        refused = refused || !warning;
    }
    if (!refused) {
        LhatCompileResult compiled = lhat_compile_next(
            prompt->compiles, &checked, &in->lexer, &in->proto);
        if (compiled.status != LHAT_COMPILE_OK) {
            // 4.2 puts the refusals in the checker, so this is a hole in it,
            // told about the way the checker's own diagnostics are.
            char text[256];
            lhat_compile_message_write(prompt->program, &compiled, text,
                                       sizeof text);
            tell(prompt, in, LHAT_REPORT_ERROR, text, compiled.offset,
                 compiled.line, compiled.column, compiled.name_length);
            refused = true;
        }
    }
    lhat_check_result_dispose(&checked);
    return !refused;
}

LhatPromptResult lhat_prompt_feed(LhatPrompt *prompt, const char *text,
                                  size_t length)
{
    LhatPromptResult result = {LHAT_PROMPT_REFUSED, lhat_nil()};
    prompt->report_length = 0;

    // 02 の 3.1 tells input that stopped early from input that is wrong, so
    // a construct spanning lines is read by asking for more rather than by
    // counting brackets.
    char *joined = (char *)realloc(prompt->input,
                                   prompt->input_length + length + 1);
    if (joined == NULL) {
        return result;
    }
    prompt->input = joined;
    memcpy(joined + prompt->input_length, text, length);
    prompt->input_length += length;
    joined[prompt->input_length] = '\0';

    if (prompt->held_count == prompt->held_capacity) {
        size_t grown = prompt->held_capacity ? prompt->held_capacity * 2 : 16;
        Held *bigger = (Held *)realloc(prompt->held, grown * sizeof *bigger);
        if (bigger == NULL) {
            return result;
        }
        prompt->held = bigger;
        prompt->held_capacity = grown;
    }
    Held *in = &prompt->held[prompt->held_count];
    if (!lhat_source_init_from_string(&in->source, prompt->name, joined,
                                      prompt->input_length)) {
        return result;
    }
    lhat_lexer_init(&in->lexer, &in->source);
    lhat_parse_interactive(&in->lexer, &in->parsed);
    in->proto = NULL;

    // 3.1: input that merely stopped early is not wrong yet, so its
    // diagnostics say nothing about what the reader will type next.
    if (in->parsed.incomplete) {
        drop_held(in);
        result.status = LHAT_PROMPT_MORE;
        return result;
    }
    lhat_prompt_drop(prompt);

    if (!accept(prompt, in)) {
        // 4.3: a refused input added nothing to either session, so dropping
        // its pieces leaves the session as it was.
        drop_held(in);
        return result;
    }

    LhatRunResult ran = lhat_run(prompt->machine, in->proto);
    prompt->held_count++;
    if (ran.status != LHAT_RUN_OK) {
        size_t needed = lhat_run_error_write(prompt->machine, ran, NULL, NULL, 0);
        char *room = report_room(prompt, needed);
        if (room != NULL) {
            prompt->report_length += lhat_run_error_write(
                prompt->machine, ran, NULL, room, needed + 1);
        }
        result.status = LHAT_PROMPT_FAILED;
        return result;
    }
    result.status = LHAT_PROMPT_RAN;
    result.value = ran.value;
    return result;
}
