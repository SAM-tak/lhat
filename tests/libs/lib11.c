#include "lhat.h"

/* function from lib1.c */
LHATMOD_DEC int lib1_export (lhat_State *L);

LHATMOD_API int lhatopen_lib11 (lhat_State *L) {
  return lib1_export(L);
}


