// L^ (lhat) -- the whole of what a host names.
//
// One header, because the rest are called source.h, value.h, token.h and the
// like: ordinary enough that putting them on somebody else's include path
// would be rude. A host puts include/ on its path, writes
//
//     #include "lhat.h"
//
// and has the language. The three below reach everything else between them:
// program.h pulls in the parser, the checker and the types, and vm.h pulls in
// the machine and its values.
//
// The order a host works in is 05 の 8.7 and 5.3:
//
//     lhat_program_init(&program, true, lhat_load_file, NULL);
//     lhat_register_func(&program, "system.io", "print", "p^string^;", fn, ctx);
//     lhat_program_check(&program, "main.lh");
//     lhat_program_compile(&program, &count);
//     lhat_machine_set_modules(machine, modules, count);
//     lhat_program_install(&program, machine);
//     lhat_run(machine, modules[root->index].proto);
//
// Registering comes before checking, because the checker has to know what a
// signature says. Installing comes before running, because that is when what
// was registered reaches L^.modules.

#ifndef LHAT_H
#define LHAT_H

// One shape for what every stage reports (03 の 1.1).
#include "../src/error.h"

// What the language asks of its surroundings: where memory comes from, and
// how a unit's text is read (05 の 8.9).
#include "../src/port.h"

// The unit graph, and what a host registers into it (05 の 5 章, 8.7, 8.8).
#include "../src/program.h"

// Compiling a unit and running one (03 の 5 章).
#include "../src/vm.h"

#endif  // LHAT_H
