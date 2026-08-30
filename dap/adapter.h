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
#include <stddef.h>
#include <stdint.h>

#include "lhat/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DapSession DapSession;

// 09 の 5.2: how the debugger's spelling of a file and the program's
// spelling of a unit meet, when they are not the same thing -- a host
// loading units out of an archive or a virtual filesystem has unit names no
// filesystem holds, and only the host knows the correspondence.
//
// `to_unit` writes the unit spelling (what LhatFrameInfo.source will say)
// of the file the editor calls `editor_path` -- setBreakpoints is matched
// through it, and false binds no breakpoints to that file. `to_editor`
// writes the editor's path for a unit -- a stack frame's source is reported
// through it, and false reports the unit spelling as it is. With no map at
// all, both sides are taken as filesystem paths and normalized.
//
// Called on the session's own threads, under its lock: quick and
// thread-safe, please.
typedef struct {
    void *context;
    bool (*to_unit)(void *context, const char *editor_path, char *out,
                    size_t capacity);
    bool (*to_editor)(void *context, const char *unit, char *out,
                      size_t capacity);
} DapPathMap;

// Listens on 127.0.0.1:`port`, waits for one debugger, and talks to it until
// it has said configurationDone -- installing the hook on `machine` (and,
// through lhat_debug_watch_machines, on every machine born after) before
// returning, so the run the caller starts next is under the debugger's
// control. `paths` maps the debugger's file spellings to the program's unit
// spellings; NULL takes both as filesystem paths. false (and `*out` NULL)
// when no debugger could be reached, which the caller treats as "run
// without one".
bool dap_session_begin(DapSession **out, LhatMachine *machine, uint16_t port,
                       const DapPathMap *paths);

// After the run: tells the debugger the program is over (with `exit_code`),
// takes the hook off, and closes. Frees the session.
void dap_session_end(DapSession *session, int exit_code);

// Whether the debugger ended the run itself (disconnect or terminate), so
// the caller shows no error of its own for the panic that stopped it.
bool dap_session_ended_run(const DapSession *session);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_DAP_ADAPTER_H
