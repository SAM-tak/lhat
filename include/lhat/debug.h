// L^ (lhat) -- what a debugger asks of a machine (09 章).
//
// Two things: to be told when the machine reaches a new line, and, while it
// is being told, to read the frames standing and what their registers were
// called. That is the whole of it. Breakpoints, stepping and the pause are a
// debugger's own, built on these -- the DAP adapter in cli/ is one, a Godot
// script debugger another -- and the machine keeps none of them.
//
// Section numbers refer to DesignDocuments/09-debugger.md unless prefixed.

#ifndef LHAT_DEBUG_H
#define LHAT_DEBUG_H

#include "lhat/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    // 2.1: the next instruction begins a line -- a line other than the last
    // one the machine was on, or the same one reached by a jump back (a
    // loop), or the first of a body just entered.
    LHAT_DEBUG_LINE
} LhatDebugEvent;

// 2.3: called on the machine's own thread, between two instructions, and
// the machine waits for it to return -- a debugger that wants to stop the
// program stops here. `where` is level 0 of lhat_machine_fault_frame,
// already read. Every register holds what the program sees, so the frame
// API below reads them as they are.
//
// 2.4: the hook may call back into L^ (lhat_machine_call and friends); it is
// not told about the lines those calls run. A fault in such a call, or
// lhat_machine_panic, ends the run the hook interrupted, the way it would
// for a host function.
typedef void (*LhatDebugHook)(LhatMachine *machine, void *context,
                              LhatDebugEvent event,
                              const LhatFrameInfo *where);

// 2.2: while set, every instruction pays one test; while not, one branch
// not taken. NULL takes it away -- from inside the hook too.
void lhat_machine_set_debug_hook(LhatMachine *machine, LhatDebugHook hook,
                                 void *context);

// 3.2: one name of a frame. A local is a written binding, a parameter, or a
// name the language binds (self^, it^, def^, super^, '...'); a capture is a
// place the body shares with the one that made it (03 の 5.4). `name` is the
// body's and is good as long as the body is. A host value comes in its
// pointer form aimed into the stack (05 の 8.9), good until the machine
// moves again.
typedef struct {
    const char *name;
    LhatValue value;
} LhatBindingInfo;

// `level` counts as lhat_machine_fault_frame's does, and reads the same
// frames: the recorded fault's, or, with none recorded, those standing right
// now -- which is what a hook, or a host function, is looking at.
//
// The locals are the ones live at the frame's instruction, in declaration
// order; of two under one name the later is the inner. What a let^ declares
// is live from the start of its block (02 の 8.7), nil^ until it runs.
size_t lhat_frame_local_count(const LhatMachine *machine, size_t level);
bool lhat_frame_local(const LhatMachine *machine, size_t level, size_t index,
                      LhatBindingInfo *out);
size_t lhat_frame_upvalue_count(const LhatMachine *machine, size_t level);
bool lhat_frame_upvalue(const LhatMachine *machine, size_t level,
                        size_t index, LhatBindingInfo *out);

// 3.4: writes one binding of a frame -- the debugger's privilege, standing
// outside anything the checker promised about the program. `index` counts
// the same bindings the readers above answer. The machine stays memory safe
// whatever ordinary value is written (a register holds any tagged value),
// but a value the body's written types did not expect can surface later as
// the runtime type error it then is.
//
// Refused -- false, nothing written -- when the level or index names
// nothing, or when the binding or the value is a host value: those are raw
// slots of a registered width (05 の 8.9), and writing across that layout
// is the one thing that would not be safe.
bool lhat_frame_set_local(LhatMachine *machine, size_t level, size_t index,
                          LhatValue value);
bool lhat_frame_set_upvalue(LhatMachine *machine, size_t level, size_t index,
                            LhatValue value);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_DEBUG_H
