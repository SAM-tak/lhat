// L^ (lhat) -- the Debug Adapter, the thing a debugger talks DAP to.
//
// It sits on the machine's line hook (lhat/debug.h): the machine keeps no
// breakpoints, so where to stop, how to step and when to pause are all here,
// decided against what the debugger asked for over the socket. One debugger,
// one machine, one run -- a VS Code extension spawns `lhat --dap=PORT` and
// connects, and this serves it.
//
// Section numbers refer to DesignDocuments/09-debugger.md unless prefixed.

#ifndef LHAT_DAP_ADAPTER_H
#define LHAT_DAP_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "lhat/vm.h"

typedef struct DapSession DapSession;

// Listens on 127.0.0.1:`port`, waits for one debugger, and talks to it until
// it has said configurationDone -- installing the hook on `machine` before
// returning, so the run the caller starts next is under the debugger's
// control. `program_path` is the script about to run, for matching source
// breakpoints against. false (and `*out` NULL) when no debugger could be
// reached, which the caller treats as "run without one".
bool dap_session_begin(DapSession **out, LhatMachine *machine, uint16_t port,
                       const char *program_path);

// After the run: tells the debugger the program is over (with `exit_code`),
// takes the hook off, and closes. Frees the session.
void dap_session_end(DapSession *session, int exit_code);

// Whether the debugger ended the run itself (disconnect or terminate), so
// the caller shows no error of its own for the panic that stopped it.
bool dap_session_ended_run(const DapSession *session);

#endif  // LHAT_DAP_ADAPTER_H
