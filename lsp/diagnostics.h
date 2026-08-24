// L^ (lhat) -- LSP server: LhatUnit diagnostics -> LSP Diagnostic[].
//
// lhat's line/column count Unicode code points (src/lexer.c). LSP's
// Position.character counts UTF-16 code units. 03-compilation-pipeline.md's
// 1.3節 already keeps two counts of a column apart -- what a diagnostic
// names (code points) and where a terminal's mark lands (cells, UAX #11).
// This file adds a third and is the only place it is computed, from the
// byte offset every one of lhat's diagnostics also carries.

#ifndef LSP_DIAGNOSTICS_H
#define LSP_DIAGNOSTICS_H

#include <stdbool.h>

#include "cJSON.h"
#include "program_internal.h"

// A cJSON array of LSP Diagnostic objects covering every lexer, parser and
// checker diagnostic `unit` carries -- 03 の 1.3's "the stage is its own,
// the rendering is shared" read onto JSON instead of text. Always an array,
// empty when there is nothing to report; NULL only on allocation failure.
//
// `project_relaxed` is host_config.h's lsp_host_config_strict, inverted --
// whether the host this unit is written for builds relaxed (03 の 3.1).
// lhatls checks every unit strict regardless, so this changes nothing about
// which diagnostics appear, only how one class of them is shown: a
// diagnostic lhat_unit_diagnostic_relaxed_ok answers true for is severity
// Warning under a relaxed host and Error otherwise, since a relaxed build
// would not stop on it. Every other diagnostic is Error either way.
cJSON *lsp_diagnostics_for_unit(const LhatUnit *unit, bool project_relaxed);

// The compile stage's one refusal ("this form does not compile yet" and
// friends), appended to `array`. The three stages above report through
// lhat_unit_diagnostic; the compiler reports through
// lhat_program_compile_failure instead, and without this the editor never
// sees it. Appends nothing while `failure` says LHAT_COMPILE_OK. The
// message is composed the way the CLI's say_compile_error composes it.
void lsp_diagnostics_add_compile_failure(cJSON *array, const LhatUnit *unit,
                                         LhatCompileResult failure);

#endif  // LSP_DIAGNOSTICS_H
