#ifndef LHAT_INSTANTIATION_INTERNAL_H
#define LHAT_INSTANTIATION_INTERNAL_H
#include "type.h"
struct LhatInstantiationContext {
    LhatTypeArena *arena;
    const LhatType *hosted;
    uint64_t transfer_arguments;
};
#endif
