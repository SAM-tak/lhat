#include "lhat.h"


LHATMOD_DEC int lhatopen_lib2 (lhat_State *L);

LHATMOD_API int lhatopen_lib21 (lhat_State *L) {
  return lhatopen_lib2(L);
}


