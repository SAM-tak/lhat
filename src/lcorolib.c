/*
** $Id: lcorolib.c,v 1.10 2016/04/11 19:19:55 roberto Exp $
** Coroutine Library
** See Copyright Notice in lhat.h
*/

#define lcorolib_c
#define LHAT_LIB

#include "lprefix.h"


#include <stdlib.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"


static lhat_State *getco (lhat_State *L) {
  lhat_State *co = lhat_tocoroutine(L, 1);
  lhatL_argcheck(L, co, 1, "coroutine expected");
  return co;
}


static int auxresume (lhat_State *L, lhat_State *co, int narg) {
  int status;
  if (!lhat_checkstack(co, narg)) {
    lhat_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  if (lhat_status(co) == LHAT_OK && lhat_gettop(co) == 0) {
    lhat_pushliteral(L, "cannot resume dead coroutine");
    return -1;  /* error flag */
  }
  lhat_xmove(L, co, narg);
  status = lhat_resume(co, L, narg);
  if (status == LHAT_OK || status == LHAT_YIELD) {
    int nres = lhat_gettop(co);
    if (!lhat_checkstack(L, nres + 1)) {
      lhat_pop(co, nres);  /* remove results anyway */
      lhat_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    lhat_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  else {
    lhat_xmove(co, L, 1);  /* move error message */
    return -1;  /* error flag */
  }
}


static int lhatB_coresume (lhat_State *L) {
  lhat_State *co = getco(L);
  int r;
  r = auxresume(L, co, lhat_gettop(L) - 1);
  if (r < 0) {
    lhat_pushboolean(L, 0);
    lhat_insert(L, -2);
    return 2;  /* return false + error message */
  }
  else {
    lhat_pushboolean(L, 1);
    lhat_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}


static int lhatB_auxwrap (lhat_State *L) {
  lhat_State *co = lhat_tocoroutine(L, lhat_upvalueindex(1));
  int r = auxresume(L, co, lhat_gettop(L));
  if (r < 0) {
    if (lhat_type(L, -1) == LHAT_TSTRING) {  /* error object is a string? */
      lhatL_where(L, 1);  /* add extra info */
      lhat_insert(L, -2);
      lhat_concat(L, 2);
    }
    return lhat_error(L);  /* propagate error */
  }
  return r;
}


static int lhatB_cocreate (lhat_State *L) {
  lhat_State *NL;
  lhatL_checktype(L, 1, LHAT_TFUNCTION);
  NL = lhat_newcoroutine(L);
  lhat_pushvalue(L, 1);  /* move function to top */
  lhat_xmove(L, NL, 1);  /* move function from L to NL */
  return 1;
}


static int lhatB_cowrap (lhat_State *L) {
  lhatB_cocreate(L);
  lhat_pushcclosure(L, lhatB_auxwrap, 1);
  return 1;
}


static int lhatB_yield (lhat_State *L) {
  return lhat_yield(L, lhat_gettop(L));
}


static int lhatB_costatus (lhat_State *L) {
  lhat_State *co = getco(L);
  if (L == co) lhat_pushliteral(L, "running");
  else {
    switch (lhat_status(co)) {
      case LHAT_YIELD:
        lhat_pushliteral(L, "suspended");
        break;
      case LHAT_OK: {
        lhat_Debug ar;
        if (lhat_getstack(co, 0, &ar) > 0)  /* does it have frames? */
          lhat_pushliteral(L, "normal");  /* it is running */
        else if (lhat_gettop(co) == 0)
            lhat_pushliteral(L, "dead");
        else
          lhat_pushliteral(L, "suspended");  /* initial state */
        break;
      }
      default:  /* some error occurred */
        lhat_pushliteral(L, "dead");
        break;
    }
  }
  return 1;
}


static int lhatB_yieldable (lhat_State *L) {
  lhat_pushboolean(L, lhat_isyieldable(L));
  return 1;
}


static int lhatB_corunning (lhat_State *L) {
  int ismain = lhat_pushcoroutine(L);
  lhat_pushboolean(L, ismain);
  return 2;
}


static const lhatL_Reg co_funcs[] = {
  {"create", lhatB_cocreate},
  {"resume", lhatB_coresume},
  {"running", lhatB_corunning},
  {"status", lhatB_costatus},
  {"wrap", lhatB_cowrap},
  {"yield", lhatB_yield},
  {"isyieldable", lhatB_yieldable},
  {NULL, NULL}
};



LHATMOD_API int lhatopen_coroutine (lhat_State *L) {
  lhatL_newlib(L, co_funcs);
  return 1;
}

