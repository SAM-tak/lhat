#include "lhat.h"
#include "lauxlib.h"

static int id (lhat_State *L) {
  return lhat_gettop(L);
}


static const struct lhatL_Reg funcs[] = {
  {"id", id},
  {NULL, NULL}
};


// function used by lib11.c
LHATMOD_API int lib1_export (lhat_State *L) {
  lhat_pushstring(L, "exported");
  return 1;
}


LHATMOD_API int onefunction (lhat_State *L) {
  lhatL_checkversion(L);
  lhat_settop(L, 2);
  lhat_pushvalue(L, 1);
  return 2;
}


LHATMOD_API int anotherfunc (lhat_State *L) {
  lhatL_checkversion(L);
  lhat_pushfstring(L, "%d%%%d\n", (int)lhat_tointeger(L, 1),
                                 (int)lhat_tointeger(L, 2));
  return 1;
} 


LHATMOD_API int lhatopen_lib1_sub (lhat_State *L) {
  lhat_setglobal(L, "y");  // 2nd arg: extra value (file name)
  lhat_setglobal(L, "x");  // 1st arg: module name
  lhatL_newlib(L, funcs);
  return 1;
}

