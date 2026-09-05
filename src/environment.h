// L^ (lhat) -- the members L^ carries (05 の 8.6).
//
// vm_machine.c's build_environment makes the values and check.c's
// environment_type the types, and the two lists have to say the same thing.
// This is the one list; each side prepares a `<name>_value` or `<name>_type`
// local and expands it, so a member added here without both halves fails to
// compile, and one added to only one side no longer compiles at all.
//
// X(name, value, type): `name` is the member's spelling (bare -- 8.1 keeps
// hat identifiers out of what a host can bind); `value` and `type` name the
// locals each expansion site prepared.

#ifndef LHAT_ENVIRONMENT_H
#define LHAT_ENVIRONMENT_H

#define LHAT_ENVIRONMENT(X)                                                  \
    X(modules, lhat_object((LhatObject *)modules_value), modules_type)       \
    X(collectgarbage, lhat_object((LhatObject *)collectgarbage_value),       \
      collectgarbage_type)

#endif  // LHAT_ENVIRONMENT_H
