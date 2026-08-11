// L^ (lhat) -- command line driver.
//
// With a file it stops after parsing or checking and prints what it found.
// With nothing to read it is the prompt of 03 の 4 章, where a machine and a
// session of each stage answer one input after another.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

// The dump modes and the diagnostic rendering read each stage's results off
// a unit directly -- the cli is one of the language's own front ends, so it
// reaches the internal headers the way lsp/ does, rather than asking the
// host API for accessors it alone would use. code.h is for --dump-bytecode,
// which writes out the instructions a host is never shown.
#include "code.h"
#include "compile.h"
#include "program_internal.h"

#ifdef LHAT_CLI_WITH_STDLIB
#include "stdlib/debug.h"
#include "stdlib/error.h"
#include "stdlib/io.h"
#include "stdlib/math.h"
#include "stdlib/random.h"
#include "stdlib/thread.h"
#endif

static void print_token(const LhatLexer *lexer, const LhatToken *token)
{
    printf("%4u:%-3u %-18s", token->line, token->column,
           lhat_token_kind_name(token->kind));

    switch (token->kind) {
        case LHAT_TOKEN_IDENT:
            printf(" %.*s", (int)token->length,
                   lexer->source->text + token->offset);
            break;
        case LHAT_TOKEN_HAT_IDENT:
            printf(" %.*s (hats=%u)", (int)token->length,
                   lexer->source->text + token->offset, token->v.hats);
            break;
        case LHAT_TOKEN_INT:
            printf(" %llu (base %u)", (unsigned long long)token->v.integer.value,
                   token->v.integer.base);
            break;
        case LHAT_TOKEN_FLOAT:
            printf(" %g", token->v.real);
            break;
        case LHAT_TOKEN_STRING: {
            size_t length = 0;
            const char *bytes = lhat_lexer_string(lexer, token, &length);
            printf(" %s \"%.*s\"", lhat_string_kind_name(token->v.string.kind),
                   (int)length, bytes != NULL ? bytes : "");
            break;
        }
        case LHAT_TOKEN_NAME_LITERAL:
        case LHAT_TOKEN_INTERP_TEXT:
        case LHAT_TOKEN_INTERP_FORMAT: {
            size_t length = 0;
            const char *bytes = lhat_lexer_string(lexer, token, &length);
            printf(" \"%.*s\"", (int)length, bytes != NULL ? bytes : "");
            break;
        }
        case LHAT_TOKEN_SCOPE:
            printf(" %s (depth=%u)", lhat_scope_kind_name(token->v.scope.kind),
                   token->v.scope.depth);
            break;
        case LHAT_TOKEN_OP:
            printf(" %s", lhat_op_name(token->v.op));
            break;
        default:
            break;
    }

    if (token->preceded_by_newline) {
        printf("   [newline before]");
    }
    printf("\n");
}

static void print_node(const LhatLexer *lexer, const LhatNode *node, int depth);

static void print_list(const LhatLexer *lexer, const char *label,
                       const LhatNode *list, int depth)
{
    if (list == NULL) {
        return;
    }
    printf("%*s%s:\n", depth * 2, "", label);
    for (const LhatNode *item = list; item != NULL; item = item->next) {
        print_node(lexer, item, depth + 1);
    }
}

static void print_node(const LhatLexer *lexer, const LhatNode *node, int depth)
{
    if (node == NULL) {
        return;
    }

    printf("%*s%s", depth * 2, "", lhat_node_kind_name(node->kind));

    switch (node->kind) {
        case LHAT_NODE_INT:
            printf(" %llu", (unsigned long long)node->v.integer.value);
            break;
        case LHAT_NODE_FLOAT:
            printf(" %g", node->v.real);
            break;
        case LHAT_NODE_STRING:
        case LHAT_NODE_NAME:
        case LHAT_NODE_INTERP_TEXT:
            printf(" \"%.*s\"", (int)node->v.string.length,
                   lexer->strings + node->v.string.offset);
            break;
        case LHAT_NODE_IDENT:
        case LHAT_NODE_HAT_IDENT:
        case LHAT_NODE_TYPE_NAME:
            printf(" %.*s", (int)node->v.name.length,
                   lexer->source->text + node->v.name.offset);
            break;
        case LHAT_NODE_SCOPE:
            printf(" %s depth=%u", lhat_scope_kind_name(node->v.scope.kind),
                   node->v.scope.depth);
            if (node->v.scope.name != NULL) {
                printf(" %.*s", (int)node->v.scope.name->v.name.length,
                       lexer->source->text + node->v.scope.name->v.name.offset);
            }
            break;
        case LHAT_NODE_UNARY:
            printf(" %s", lhat_op_name(node->v.unary.op));
            break;
        case LHAT_NODE_BINARY:
        case LHAT_NODE_TYPE_UNION:
        case LHAT_NODE_TYPE_INTERSECT:
            if (node->kind == LHAT_NODE_BINARY) {
                printf(" %s", lhat_op_name(node->v.binary.op));
            }
            break;
        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX:
        case LHAT_NODE_CALL:
            if (node->v.access.nil_safe) {
                printf(" (nil-safe)");
            }
            break;
        case LHAT_NODE_FUNC:
        case LHAT_NODE_TYPE_FUNC:
            printf(" %s%s", node->v.func.is_function ? "f^" : "p^",
                   node->v.func.yields ? " yieldable" : "");
            break;
        case LHAT_NODE_PARAM:
            if (node->v.param.variadic) {
                printf(" variadic");
            }
            break;
        case LHAT_NODE_TABLE_ENTRY:
            if (node->v.entry.modifier == LHAT_DEF_OVERRIDE) {
                printf(" override^");
            } else if (node->v.entry.modifier == LHAT_DEF_OVERLOAD) {
                printf(" overload^");
            }
            break;
        case LHAT_NODE_YIELD:
            if (node->v.jump.phantom) {
                printf(" phantom");  // 15.11
            }
            break;
        case LHAT_NODE_DEFINE:
            if (node->v.binding.exported) {
                printf(" public^");
            }
            break;
        case LHAT_NODE_ERRORDEF:
            if (node->v.named.exported) {
                printf(" public^");
            }
            break;
        case LHAT_NODE_FOR: {
            static const char *const kinds[] = {
                "to^", "downto^", "in^", "while^", "until^", "if^", "when^",
                "do^"
            };
            printf(" %s", kinds[node->v.loop.kind]);
            break;
        }
        case LHAT_NODE_REPEAT: {
            static const char *const kinds[] = {
                "forever", "count", "while^", "until^"
            };
            printf(" %s", kinds[node->v.repeat.kind]);
            break;
        }
        case LHAT_NODE_LOOP_CLAUSE: {
            static const char *const kinds[] = {
                "prolog^", "pre^", "first^", "main^", "last^", "epilog^",
                "finally^"
            };
            printf(" %s", kinds[node->v.loop_clause.kind]);
            break;
        }
        default:
            break;
    }
    printf("\n");

    switch (node->kind) {
        case LHAT_NODE_UNARY:
            print_node(lexer, node->v.unary.operand, depth + 1);
            break;
        case LHAT_NODE_BINARY:
        case LHAT_NODE_TYPE_UNION:
        case LHAT_NODE_TYPE_INTERSECT:
            print_node(lexer, node->v.binary.left, depth + 1);
            print_node(lexer, node->v.binary.right, depth + 1);
            break;
        case LHAT_NODE_COMPARE_CHAIN:
            print_list(lexer, "operands", node->v.chain.operands, depth + 1);
            print_list(lexer, "operators", node->v.chain.operators, depth + 1);
            break;
        case LHAT_NODE_MEMBER:
        case LHAT_NODE_INDEX:
            print_node(lexer, node->v.access.target, depth + 1);
            print_node(lexer, node->v.access.argument, depth + 1);
            break;
        case LHAT_NODE_CALL:
            print_node(lexer, node->v.access.target, depth + 1);
            print_list(lexer, "args", node->v.access.argument, depth + 1);
            break;
        case LHAT_NODE_AS:
            print_node(lexer, node->v.ascription.value, depth + 1);
            print_node(lexer, node->v.ascription.type, depth + 1);
            break;
        case LHAT_NODE_FUNC:
        case LHAT_NODE_TYPE_FUNC:
            print_list(lexer, "params", node->v.func.params, depth + 1);
            if (node->v.func.return_type != NULL) {
                printf("%*sreturns:\n", (depth + 1) * 2, "");
                print_node(lexer, node->v.func.return_type, depth + 2);
            }
            print_node(lexer, node->v.func.body, depth + 1);
            break;
        case LHAT_NODE_PARAM:
            print_node(lexer, node->v.param.name, depth + 1);
            print_node(lexer, node->v.param.type, depth + 1);
            print_node(lexer, node->v.param.fallback, depth + 1);
            break;
        case LHAT_NODE_TABLE_ENTRY:
        case LHAT_NODE_MEMBER_DECL:
            print_node(lexer, node->v.entry.key, depth + 1);
            print_node(lexer, node->v.entry.value, depth + 1);
            break;
        case LHAT_NODE_DEFINE:
        case LHAT_NODE_REASSIGN:
            print_list(lexer, "targets", node->v.binding.targets, depth + 1);
            print_list(lexer, "values", node->v.binding.values, depth + 1);
            break;
        case LHAT_NODE_IF_CLAUSE:
            print_node(lexer, node->v.clause.condition, depth + 1);
            print_node(lexer, node->v.clause.body, depth + 1);
            break;
        case LHAT_NODE_INTERP_HOLE:
            print_node(lexer, node->v.hole.value, depth + 1);
            print_node(lexer, node->v.hole.format, depth + 1);
            break;
        // 13.8改: return^ may carry several values, and one is a list of one.
        case LHAT_NODE_RETURN:
        case LHAT_NODE_YIELD:
            print_list(lexer, "values", node->v.jump.value, depth + 1);
            break;
        case LHAT_NODE_BREAK:
        case LHAT_NODE_CALL_STMT:
        case LHAT_NODE_PACK:
        case LHAT_NODE_REQUIRE:
        case LHAT_NODE_REQUIRE_STMT:
        case LHAT_NODE_IMPORT:
        case LHAT_NODE_IMPORT_STMT:
        case LHAT_NODE_TRY:
            print_node(lexer, node->v.jump.value, depth + 1);
            break;
        case LHAT_NODE_TYPE_CORO:
            print_node(lexer, node->v.coroutine.receive, depth + 1);
            print_node(lexer, node->v.coroutine.produce, depth + 1);
            print_node(lexer, node->v.coroutine.result, depth + 1);
            break;
        case LHAT_NODE_BLOCK:
            print_list(lexer, "items", node->v.list.items, depth + 1);
            print_list(lexer, "clauses", node->v.list.extra, depth + 1);
            break;
        case LHAT_NODE_MODULE:
        case LHAT_NODE_ERROR_KIND:
        case LHAT_NODE_ERROR_NEW:
            print_node(lexer, node->v.named.name, depth + 1);
            print_list(lexer, "members", node->v.named.members, depth + 1);
            break;
        case LHAT_NODE_TABLE:
        case LHAT_NODE_DEF:
        case LHAT_NODE_SELF_TABLE:
        case LHAT_NODE_IF_STMT:
        case LHAT_NODE_IF_EXPR:
        case LHAT_NODE_INTERP:
        case LHAT_NODE_TYPE_TABLE:
        case LHAT_NODE_TYPE_TUPLE:  // 13.8改: the positions, in order
        case LHAT_NODE_TUPLE:
            print_list(lexer, "items", node->v.list.items, depth + 1);
            break;
        case LHAT_NODE_FOR:
            print_list(lexer, "focus", node->v.loop.focus, depth + 1);
            print_node(lexer, node->v.loop.bound, depth + 1);
            print_node(lexer, node->v.loop.step, depth + 1);
            print_list(lexer, "next", node->v.loop.advance, depth + 1);
            print_node(lexer, node->v.loop.body, depth + 1);
            break;
        case LHAT_NODE_REPEAT:
            print_node(lexer, node->v.repeat.bound, depth + 1);
            print_node(lexer, node->v.repeat.body, depth + 1);
            break;
        case LHAT_NODE_LOOP_CLAUSE:
            print_list(lexer, "body", node->v.loop_clause.body, depth + 1);
            break;
        case LHAT_NODE_WITH:
            print_list(lexer, "bindings", node->v.list.items, depth + 1);
            print_node(lexer, node->v.list.extra, depth + 1);
            break;
        default:
            break;
    }
}

// Whether a diagnostic is shown with the line it happened on. The rich form
// wants the source, so it is off wherever this driver has none to hand.
static bool rich_reports = true;

// error.h fills a buffer rather than a stream, so the driver decides where it
// goes -- 05 の 8.9 keeps the language away from stdio.
static void say(const LhatReport *report, const LhatSource *source,
                const char *name)
{
    char room[1024];
    size_t needed = lhat_report_write(report, source, name, rich_reports, room,
                                      sizeof room);
    if (needed < sizeof room) {
        fprintf(stderr, "%s\n", room);
        return;
    }
    char *bigger = (char *)malloc(needed + 1);
    if (bigger == NULL) {
        fprintf(stderr, "%s\n", room);  // truncated, but better than silence
        return;
    }
    lhat_report_write(report, source, name, rich_reports, bigger, needed + 1);
    fprintf(stderr, "%s\n", bigger);
    free(bigger);
}

// The parser's message can name the token it wanted, which no literal can
// carry, so it is written into a buffer first and the report borrows that.
static void say_parse_error(const LhatSource *source, const char *name,
                            const LhatParseDiagnostic *d)
{
    char message[256];
    size_t needed = lhat_parse_message_write(d, message, sizeof message);
    char *text = message;
    char *bigger = NULL;
    if (needed >= sizeof message) {
        bigger = (char *)malloc(needed + 1);
        if (bigger != NULL) {
            lhat_parse_message_write(d, bigger, needed + 1);
            text = bigger;
        }
    }

    LhatReport report;
    report.kind = LHAT_REPORT_ERROR;
    report.message = text;
    report.offset = d->offset;
    report.line = d->line;
    report.column = d->column;
    report.length = d->length;
    say(&report, source, name);
    free(bigger);
}

// The same for the checker, whose codes about a name can say which.
static void say_check_error(const LhatSource *source, const char *name,
                            const LhatCheckDiagnostic *d)
{
    char message[256];
    size_t needed = lhat_check_message_write(d, message, sizeof message);
    char *text = message;
    char *bigger = NULL;
    if (needed >= sizeof message) {
        bigger = (char *)malloc(needed + 1);
        if (bigger != NULL) {
            lhat_check_message_write(d, bigger, needed + 1);
            text = bigger;
        }
    }

    LhatReport report;
    report.kind = LHAT_REPORT_ERROR;
    report.message = text;
    report.offset = d->offset;
    report.line = d->line;
    report.column = d->column;
    report.length = d->name_length;
    say(&report, source, name);
    free(bigger);
}

// 04 の 11.6改: what the program wrote in panic^ EXPR, as text -- a plain
// string prints as its own text (a message reads oddly in quotes), anything
// else falls back to lhat_value_write's general form.
static void say_panic_value(LhatValue value)
{
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) {
        const LhatString *text = (const LhatString *)lhat_as_object(value);
        fprintf(stderr, "%.*s", (int)text->length, text->text);
        return;
    }
    size_t needed = lhat_value_write(value, NULL, 0);
    char *room = (char *)malloc(needed + 1);
    if (room == NULL) {
        return;
    }
    lhat_value_write(value, room, needed + 1);
    fprintf(stderr, "%s", room);
    free(room);
}

// 04 の 11 章: a runtime fault names the line it happened on and, when it was
// one, the operator -- both from LhatRunResult, not the richer source-offset
// diagnostics the checker/parser give (those need a source and a token; a
// fault only ever has a line).
static void say_run_error(const char *path, LhatRunResult ran)
{
    if (path != NULL) {
        fprintf(stderr, "%s: ", path);
    }
    fprintf(stderr, "error: ");
    if (ran.line > 0) {
        fprintf(stderr, "line %u: ", ran.line);
    }
    if (ran.status == LHAT_RUN_PANIC) {
        say_panic_value(ran.value);
        fprintf(stderr, "\n");
        return;
    }
    fprintf(stderr, "%s", lhat_run_status_message(ran.status));
    if (ran.op_name != NULL) {
        fprintf(stderr, " (%.*s)", (int)ran.op_name_length, ran.op_name);
    }
    fprintf(stderr, "\n");
}

static void say_error(const LhatSource *source, const char *name,
                      uint32_t offset, uint32_t line, uint32_t column,
                      const char *message)
{
    LhatReport report;
    report.kind = LHAT_REPORT_ERROR;
    report.message = message;
    report.offset = offset;
    report.line = line;
    report.column = column;
    report.length = 0;
    say(&report, source, name);
}

static int report_lexical(const LhatLexer *lexer, const LhatSource *source)
{
    int status = EXIT_SUCCESS;
    for (size_t i = 0; i < lexer->diagnostic_count; i++) {
        const LhatDiagnostic *d = &lexer->diagnostics[i];
        say_error(source, NULL, d->offset, d->line, d->column,
                  lhat_lexer_error_message(d->code));
        status = EXIT_FAILURE;
    }
    return status;
}

static int dump_tokens(const LhatSource *source)
{
    LhatLexer lexer;
    lhat_lexer_init(&lexer, source);

    for (;;) {
        LhatToken token = lhat_lexer_next(&lexer);
        print_token(&lexer, &token);
        if (token.kind == LHAT_TOKEN_EOF) {
            break;
        }
    }

    int status = report_lexical(&lexer, source);
    lhat_lexer_dispose(&lexer);
    return status;
}

// 05 の 8.2's example, and the only name this driver hands out. 02 の 10.6
// calls this the one exception the f^ constraints allow: what it changes is
// the host's, not the machine's, so nothing a program can read afterwards
// tells it apart from having done nothing. 13.2 has an f^ answer on every
// path, so it answers nil^ -- which 04 の 11.3 already spells "nothing here".
//
// 13.7's '...', so what was handed over is written in the order it came, one
// line each. The reading is 02 の 14.17's tostring, lhat_value_text -- a
// string^ is its own bytes, without the quotes that let 1 and "1" be read
// apart (those belong to 03 の 4 章's prompt, not to a writer). The same
// print stdlib/io.c registers as std.io.print.
static LhatValue host_print(LhatMachine *machine, void *context,
                            const LhatValue *arguments, size_t count)
{
    (void)machine;
    (void)context;
    for (size_t i = 0; i < count; i++) {
        size_t needed = lhat_value_text(arguments[i], NULL, 0);
        char *text = (char *)malloc(needed + 1);
        if (text != NULL) {
            lhat_value_text(arguments[i], text, needed + 1);
            fwrite(text, 1, needed, stdout);
            fputc('\n', stdout);
            free(text);
        }
    }
    return lhat_nil();
}

// 05 の 8.2: the host decides what a program sees without a require^. This
// driver binds print, and collectgarbage because 8.6 already puts it in L^ --
// writing both out is what shows the two are the same mechanism.
static bool bind_host_names(LhatProgram *program)
{
    lhat_register_global(program, "print", "f^...->nil^;", host_print, NULL);
    lhat_bind_initial(program, "print", "L^.print");
    lhat_bind_initial(program, "collectgarbage", "L^.collectgarbage");

#ifdef LHAT_CLI_WITH_STDLIB
    if (!lhatstdlib_io_register(program) ||
        !lhatstdlib_thread_register(program) ||
        !lhatstdlib_random_register(program) ||
        !lhatstdlib_math_register(program) ||
        !lhatstdlib_debug_register(program)) {
        return false;
    }
#endif

    return true;
}

// 03 の 1.1's three stages, each with its own codes and one shape to show
// them in. Answers how many units the graph reached.
static size_t say_unit_diagnostics(const LhatProgram *program)
{
    for (size_t i = 0; i < program->diagnostic_count; i++) {
        const LhatProgramDiagnostic *d = &program->diagnostics[i];
        fprintf(stderr, "%s: error: %s\n", d->path,
                lhat_program_error_message(d->code));
    }

    size_t units = 0;
    for (const LhatUnit *unit = program->units; unit != NULL;
         unit = unit->next) {
        units++;
        if (!unit->loaded) {
            continue;
        }
        for (size_t i = 0; i < unit->lexer.diagnostic_count; i++) {
            const LhatDiagnostic *d = &unit->lexer.diagnostics[i];
            say_error(&unit->source, unit->path, d->offset, d->line, d->column,
                      lhat_lexer_error_message(d->code));
        }
        for (size_t i = 0; i < unit->parsed.diagnostic_count; i++) {
            const LhatParseDiagnostic *d = &unit->parsed.diagnostics[i];
            say_parse_error(&unit->source, unit->path, d);
        }
        for (size_t i = 0; i < unit->checked.diagnostic_count; i++) {
            const LhatCheckDiagnostic *d = &unit->checked.diagnostics[i];
            say_check_error(&unit->source, unit->path, d);
        }
    }
    return units;
}

// 03 の 1.1's third stage, over the unit graph of 05 の 6.2: a unit cannot be
// checked before the units it requires, so the whole graph is walked rather
// than the one file named on the command line.
static int check_program(const char *path, bool run, bool strict)
{
    LhatProgram program;
    lhat_program_init(&program, strict, lhat_load_file, NULL);
    if (!bind_host_names(&program)) {  // 05 の 8.2, before checking (8.3)
        fprintf(stderr, "lhat: out of memory\n");
        lhat_program_dispose(&program);
        return EXIT_FAILURE;
    }

    const LhatUnit *root = lhat_program_check(&program, path);
    size_t units = say_unit_diagnostics(&program);

    bool failed = root == NULL || lhat_program_has_errors(&program);
    if (!failed && !run) {
        printf("%s: no type errors (%zu unit%s)\n", path, units,
               units == 1 ? "" : "s");
    }

    // 05 の 5.3: every unit compiles, and the machine is given the lot so a
    // require^ inside one can reach another.
    if (!failed && run) {
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        LhatMachine *machine = modules != NULL ? lhat_machine_new() : NULL;
        if (machine == NULL) {
            // A unit that would not compile says which form stopped it; only
            // a machine that could not be made has nothing more to add.
            if (program.compile_status != LHAT_COMPILE_OK) {
                fprintf(stderr, "%s: error: %s\n", path,
                        lhat_compile_status_message(program.compile_status));
            } else {
                fprintf(stderr, "%s: error: the program did not compile\n",
                        path);
            }
            failed = true;
        } else {
            lhat_machine_set_modules(machine, modules, count);
            // 05 の 8.7: what was registered reaches the machine here, which
            // is what makes the names bound above answer something.
            lhat_program_install(&program, machine);
            LhatRunResult ran = lhat_run(machine, modules[root->index].proto);
            if (ran.status != LHAT_RUN_OK) {
                say_run_error(path, ran);
                failed = true;
            } else if (!lhat_is_nil(ran.value)) {
                size_t needed = lhat_value_write(ran.value, NULL, 0);
                char *text = (char *)malloc(needed + 1);
                if (text != NULL) {
                    lhat_value_write(ran.value, text, needed + 1);
                    printf("%s\n", text);
                    free(text);
                }
            }
            lhat_machine_dispose(machine);
        }
    }

    lhat_program_dispose(&program);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int dump_tree(const LhatSource *source, bool typed, bool command)
{
    LhatLexer lexer;
    lhat_lexer_init(&lexer, source);

    LhatParseResult result;
    if (command) {
        lhat_parse_command(&lexer, &result);
    } else {
        lhat_parse(&lexer, &result);
    }

    if (!typed) {
        print_node(&lexer, result.root, 0);
    }

    int status = report_lexical(&lexer, source);
    for (size_t i = 0; i < result.diagnostic_count; i++) {
        const LhatParseDiagnostic *d = &result.diagnostics[i];
        say_parse_error(source, NULL, d);
        status = EXIT_FAILURE;
    }
    if (result.incomplete) {
        fprintf(stderr, "%s: note: the input ended before the construct was "
                        "complete\n", source->name);
    }

    lhat_parse_result_dispose(&result);
    lhat_lexer_dispose(&lexer);
    return status;
}

// One compiled body, then the bodies written inside it, one step deeper --
// the shape 03 の 5.2 gives a unit. lhat_chunk_print writes the instruction;
// what is added here is the line column, since a chunk keeps one source line
// per instruction (04 の 11 章).
static void print_proto(const LhatProto *proto, size_t number, int depth)
{
    printf("%*sbody %zu: %u registers", depth * 2, "", number,
           proto->chunk.registers);
    if (proto->parameters != 0) {
        printf(", %u parameters", proto->parameters);
    }
    if (proto->yields) {
        printf(", yields");
    }
    printf("\n");
    for (size_t i = 0; i < proto->chunk.count; i++) {
        char text[96];
        lhat_chunk_print(&proto->chunk, i, text, sizeof text);
        printf("%*s%4zu  line %-4u  %s\n", depth * 2, "", i,
               proto->chunk.lines[i], text);
    }
    for (size_t i = 0; i < proto->proto_count; i++) {
        print_proto(proto->protos[i], i + 1, depth + 1);
    }
}

// The compiled form of the whole program: the same graph walk as
// check_program, ending at the instructions instead of the run. Checking
// comes first because 05 の 5.3 compiles every unit the root requires, and
// the graph is only known once it has been walked -- relaxed, since the dump
// is after the instructions, not the diagnoses (03 の 4.2 emits the same
// ones either way).
static int dump_bytecode(const char *path)
{
    LhatProgram program;
    lhat_program_init(&program, false, lhat_load_file, NULL);
    if (!bind_host_names(&program)) {  // 05 の 8.2, before checking (8.3)
        fprintf(stderr, "lhat: out of memory\n");
        lhat_program_dispose(&program);
        return EXIT_FAILURE;
    }

    const LhatUnit *root = lhat_program_check(&program, path);
    say_unit_diagnostics(&program);

    bool failed = root == NULL || lhat_program_has_errors(&program);
    if (!failed) {
        size_t count = 0;
        const LhatModule *modules = lhat_program_compile(&program, &count);
        if (modules == NULL) {
            if (program.compile_status != LHAT_COMPILE_OK) {
                fprintf(stderr, "%s: error: %s\n", path,
                        lhat_compile_status_message(program.compile_status));
            }
            failed = true;
        } else {
            for (size_t i = 0; i < count; i++) {
                printf("%s\n", modules[i].module_name != NULL
                                   ? modules[i].module_name
                                   : path);
                print_proto(modules[i].proto, 0, 0);
            }
        }
    }

    lhat_program_dispose(&program);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

// 03 の 4 章: one machine and one session of each stage, answering many
// inputs. 4.3 keeps the names of one input for the next; 02 の 8.2 makes a
// bare expression a statement here and nowhere else.
static int repl(bool strict)
{
    LhatMachine *machine = lhat_machine_new();
    LhatCheckSession *checks = lhat_check_session_new();
    LhatCompileSession *compiles = lhat_compile_session_new();
    if (machine == NULL || checks == NULL || compiles == NULL) {
        fprintf(stderr, "lhat: out of memory\n");
        return EXIT_FAILURE;
    }

    // 05 の 8.7: a prompt has no program driving it, so it is given one that
    // holds nothing but the registrations -- with no loader, since 5.3 gives a
    // require^ here nowhere to go regardless. That is what lets a prompt see
    // exactly what a file sees: the same bind_host_names, hence the same
    // stdlib. It outlives both sessions, which borrow its types.
    LhatProgram program;
    lhat_program_init(&program, strict, NULL, NULL);
    if (!bind_host_names(&program)) {  // 05 の 8.2 and 8.7
        fprintf(stderr, "lhat: out of memory\n");
        lhat_program_dispose(&program);
        return EXIT_FAILURE;
    }
    lhat_program_install_checks(&program, checks);
    lhat_program_install_compiles(&program, compiles);
    if (!lhat_program_install(&program, machine)) {
        fprintf(stderr, "lhat: out of memory\n");
        lhat_program_dispose(&program);
        return EXIT_FAILURE;
    }

    printf("L^ (lhat) %s\n", LHAT_VERSION);
    printf("an expression on its own is answered; an unfinished construct "
           "reads on\n");
    printf("ctrl-d or an empty line ends\n");

    // Every input's pieces have to outlive the run -- a proto is what the
    // machine runs, and names point into the lexer's source. The session
    // keeps them all until it ends.
    size_t held = 0;
    size_t capacity = 0;
    struct Held {
        LhatSource source;
        LhatLexer lexer;
        LhatParseResult parsed;
        LhatProto *proto;
    } *kept = NULL;

    char line[4096];
    char *input = NULL;
    size_t input_length = 0;
    bool carrying = false;  // the input so far stopped early (02 の 3.1)

    for (;;) {
        printf(carrying ? ". " : "> ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) {
            printf("\n");
            break;
        }
        if (line[0] == '\n' || line[0] == '\r') {
            if (!carrying) {
                break;
            }
            // A construct being carried has no end in sight, so this is the
            // way out of one the reader will never finish.
            fprintf(stderr, "note: the unfinished input was dropped\n");
            free(input);
            input = NULL;
            input_length = 0;
            carrying = false;
            continue;
        }

        // 02 の 3.1 tells input that stopped early from input that is wrong,
        // so a construct spanning lines is read by asking for more rather
        // than by counting brackets here.
        size_t added = strlen(line);
        char *joined = (char *)realloc(input, input_length + added + 1);
        if (joined == NULL) {
            fprintf(stderr, "lhat: out of memory\n");
            break;
        }
        input = joined;
        memcpy(input + input_length, line, added + 1);
        input_length += added;

        if (held == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            void *bigger = realloc(kept, grown * sizeof *kept);
            if (bigger == NULL) {
                fprintf(stderr, "lhat: out of memory\n");
                break;
            }
            kept = bigger;
            capacity = grown;
        }
        struct Held *in = &kept[held];

        lhat_source_init_from_string(&in->source, "<stdin>", input,
                                     input_length);
        lhat_lexer_init(&in->lexer, &in->source);
        lhat_parse_interactive(&in->lexer, &in->parsed);
        in->proto = NULL;

        // 3.1: input that merely stopped early is not wrong yet, so its
        // diagnostics say nothing about what the reader will type next.
        if (in->parsed.incomplete) {
            lhat_parse_result_dispose(&in->parsed);
            lhat_lexer_dispose(&in->lexer);
            lhat_source_dispose(&in->source);
            carrying = true;
            continue;
        }
        free(input);
        input = NULL;
        input_length = 0;
        carrying = false;

        bool refused = in->lexer.diagnostic_count > 0;
        for (size_t i = 0; i < in->lexer.diagnostic_count; i++) {
            const LhatDiagnostic *d = &in->lexer.diagnostics[i];
            say_error(&in->source, "stdin", d->offset, d->line, d->column,
                      lhat_lexer_error_message(d->code));
        }
        for (size_t i = 0; i < in->parsed.diagnostic_count; i++) {
            const LhatParseDiagnostic *d = &in->parsed.diagnostics[i];
            say_parse_error(&in->source, "stdin", d);
            refused = true;
        }

        LhatCheckResult checked;
        if (!refused) {
            lhat_check_next(checks, in->parsed.root, &in->lexer, strict,
                            &checked);
            for (size_t i = 0; i < checked.diagnostic_count; i++) {
                const LhatCheckDiagnostic *d = &checked.diagnostics[i];
                say_check_error(&in->source, "stdin", d);
                refused = true;
            }
            lhat_check_result_dispose(&checked);
        }

        if (!refused) {
            LhatCompileStatus status = lhat_compile_next(
                compiles, in->parsed.root, &in->lexer, &in->proto);
            if (status != LHAT_COMPILE_OK) {
                fprintf(stderr, "error: %s\n",
                        lhat_compile_status_message(status));
                refused = true;
            }
        }

        if (refused) {
            // 4.3: a refused input added nothing to either session, so
            // dropping its pieces here leaves the session as it was.
            lhat_proto_free(in->proto);
            lhat_parse_result_dispose(&in->parsed);
            lhat_lexer_dispose(&in->lexer);
            lhat_source_dispose(&in->source);
            continue;
        }

        LhatRunResult ran = lhat_run(machine, in->proto);
        held++;
        if (ran.status != LHAT_RUN_OK) {
            say_run_error(NULL, ran);
            continue;
        }
        if (!lhat_is_nil(ran.value)) {
            size_t needed = lhat_value_write(ran.value, NULL, 0);
            char *text = (char *)malloc(needed + 1);
            if (text != NULL) {
                lhat_value_write(ran.value, text, needed + 1);
                printf("%s\n", text);
                free(text);
            }
        }
    }

    free(input);
    for (size_t i = 0; i < held; i++) {
        lhat_proto_free(kept[i].proto);
        lhat_parse_result_dispose(&kept[i].parsed);
        lhat_lexer_dispose(&kept[i].lexer);
        lhat_source_dispose(&kept[i].source);
    }
    free(kept);
    lhat_compile_session_dispose(compiles);
    lhat_check_session_dispose(checks);
    lhat_machine_dispose(machine);
    // Last: the sessions above borrowed its types and its tags.
    lhat_program_dispose(&program);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    bool tokens_only = false;
    bool bytecode_only = false;
    bool check_only = false;
    bool run_program = false;
    bool command_form = false;
    // 03 の 3.1: a file defaults to strict, the prompt to relaxed. Writing
    // the other one out overrides whichever default the mode below picks.
    enum { STRICTNESS_DEFAULT, STRICTNESS_STRICT, STRICTNESS_RELAXED }
        strictness = STRICTNESS_DEFAULT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            tokens_only = true;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            bytecode_only = true;
        } else if (strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--run") == 0) {
            run_program = true;
        } else if (strcmp(argv[i], "--command") == 0) {
            command_form = true;
        } else if (strcmp(argv[i], "--strict") == 0) {
            strictness = STRICTNESS_STRICT;
        } else if (strcmp(argv[i], "--relaxed") == 0) {
            strictness = STRICTNESS_RELAXED;
        } else {
            path = argv[i];
        }
    }

    // 03 の 4 章: with nothing to read, read from the prompt.
    if (path == NULL && !tokens_only && !bytecode_only && !check_only &&
        !run_program && !command_form) {
        return repl(strictness == STRICTNESS_STRICT);
    }

    if (path == NULL) {
        printf("L^ (lhat) %s\n", LHAT_VERSION);
        printf("usage: lhat [option] <file>\n");
        printf("  no file        read from a prompt (03 の 4 章)\n");
        printf("  --run          check the whole program and run it"
                                " (05 の 5.3)\n");
        printf("  --check        type check and report, without running\n");
        printf("  default        print the syntax tree\n");
        printf("  --tokens       print the token stream instead\n");
        printf("  --dump-bytecode  print what the unit compiles to"
                                  " (03 の 5 章)\n");
        printf("  --command      read the input as the command form"
                                " (02 の 2 章)\n");
        printf("  --strict       report a type error at compile time"
                                " (03 の 3.1; default for a file)\n");
        printf("  --relaxed      leave an undecided type to a runtime check"
                                " (03 の 3.1; default for the prompt)\n");
        return EXIT_SUCCESS;
    }

    // Checking is a question about a program, not about a file: 05 の 6.2
    // puts the units a file requires ahead of it. The bytecode dump walks
    // the same graph, so it reads its own input the same way.
    if (check_only || run_program) {
        return check_program(path, run_program,
                             strictness != STRICTNESS_RELAXED);
    }
    if (bytecode_only) {
        return dump_bytecode(path);
    }

    LhatSource source;
    char *error = NULL;
    if (!lhat_source_init_from_file(&source, path, &error)) {
        fprintf(stderr, "lhat: %s\n", error != NULL ? error : "cannot read input");
        free(error);
        return EXIT_FAILURE;
    }

    int status = tokens_only ? dump_tokens(&source)
                             : dump_tree(&source, false, command_form);
    lhat_source_dispose(&source);
    return status;
}
