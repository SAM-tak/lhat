#include "lhat.h"
#include "lauxlib.h"

static int id (lhat_State *L) {
  return lhat_gettop(L);
}


static const struct lhatL_Reg funcs[] = {
  {"id", id},
  {NULL, NULL}
};


LHATMOD_API int lhatopen_lib2 (lhat_State *L) {
  lhat_settop(L, 2);
  lhat_setglobal(L, "y");  // y gets 2nd parameter
  lhat_setglobal(L, "x");  // x gets 1st parameter
  lhatL_newlib(L, funcs);
  return 1;
}


