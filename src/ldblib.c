/*
** $Id: ldblib.c,v 1.151 2015/11/23 11:29:43 roberto Exp $
** Interface from Lhat to its debug API
** See Copyright Notice in lhat.h
*/

#define ldblib_c
#define LHAT_LIB

#include "lprefix.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"


/*
** The hook table at registry[&HOOKKEY] maps coroutines to their current
** hook function. (We only need the unique address of 'HOOKKEY'.)
*/
static const int HOOKKEY = 0;


/*
** If L1 != L, L1 can be in any state, and therefore there are no
** guarantees about its stack space; any push in L1 must be
** checked.
*/
static void checkstack (lhat_State *L, lhat_State *L1, int n) {
  if (L != L1 && !lhat_checkstack(L1, n))
    lhatL_error(L, "stack overflow");
}


static int db_getregistry (lhat_State *L) {
  lhat_pushvalue(L, LHAT_REGISTRYINDEX);
  return 1;
}


static int db_getmetatable (lhat_State *L) {
  lhatL_checkany(L, 1);
  if (!lhat_getmetatable(L, 1)) {
    lhat_pushnil(L);  /* no metatable */
  }
  return 1;
}


static int db_setmetatable (lhat_State *L) {
  int t = lhat_type(L, 2);
  lhatL_argcheck(L, t == LHAT_TNIL || t == LHAT_TTABLE, 2,
                    "nil or table expected");
  lhat_settop(L, 2);
  lhat_setmetatable(L, 1);
  return 1;  /* return 1st argument */
}


static int db_getuservalue (lhat_State *L) {
  if (lhat_type(L, 1) != LHAT_TUSERDATA)
    lhat_pushnil(L);
  else
    lhat_getuservalue(L, 1);
  return 1;
}


static int db_setuservalue (lhat_State *L) {
  lhatL_checktype(L, 1, LHAT_TUSERDATA);
  lhatL_checkany(L, 2);
  lhat_settop(L, 2);
  lhat_setuservalue(L, 1);
  return 1;
}


/*
** Auxiliary function used by several library functions: check for
** an optional coroutine as function's first argument and set 'arg' with
** 1 if this argument is present (so that functions can skip it to
** access their other arguments)
*/
static lhat_State *getcoroutine (lhat_State *L, int *arg) {
  if (lhat_iscoroutine(L, 1)) {
    *arg = 1;
    return lhat_tocoroutine(L, 1);
  }
  else {
    *arg = 0;
    return L;  /* function will operate over current coroutine */
  }
}


/*
** Variations of 'lhat_settable', used by 'db_getinfo' to put results
** from 'lhat_getinfo' into result table. Key is always a string;
** value can be a string, an int, or a boolean.
*/
static void settabss (lhat_State *L, const char *k, const char *v) {
  lhat_pushstring(L, v);
  lhat_setfield(L, -2, k);
}

static void settabsi (lhat_State *L, const char *k, int v) {
  lhat_pushinteger(L, v);
  lhat_setfield(L, -2, k);
}

static void settabsb (lhat_State *L, const char *k, int v) {
  lhat_pushboolean(L, v);
  lhat_setfield(L, -2, k);
}


/*
** In function 'db_getinfo', the call to 'lhat_getinfo' may push
** results on the stack; later it creates the result table to put
** these objects. Function 'treatstackoption' puts the result from
** 'lhat_getinfo' on top of the result table so that it can call
** 'lhat_setfield'.
*/
static void treatstackoption (lhat_State *L, lhat_State *L1, const char *fname) {
  if (L == L1)
    lhat_rotate(L, -2, 1);  /* exchange object and table */
  else
    lhat_xmove(L1, L, 1);  /* move object to the "main" stack */
  lhat_setfield(L, -2, fname);  /* put object into table */
}


/*
** Calls 'lhat_getinfo' and collects all results in a new table.
** L1 needs stack space for an optional input (function) plus
** two optional outputs (function and line table) from function
** 'lhat_getinfo'.
*/
static int db_getinfo (lhat_State *L) {
  lhat_Debug ar;
  int arg;
  lhat_State *L1 = getcoroutine(L, &arg);
  const char *options = lhatL_optstring(L, arg+2, "flnStu");
  checkstack(L, L1, 3);
  if (lhat_isfunction(L, arg + 1)) {  /* info about a function? */
    options = lhat_pushfstring(L, ">%s", options);  /* add '>' to 'options' */
    lhat_pushvalue(L, arg + 1);  /* move function to 'L1' stack */
    lhat_xmove(L, L1, 1);
  }
  else {  /* stack level */
    if (!lhat_getstack(L1, (int)lhatL_checkinteger(L, arg + 1), &ar)) {
      lhat_pushnil(L);  /* level out of range */
      return 1;
    }
  }
  if (!lhat_getinfo(L1, options, &ar))
    return lhatL_argerror(L, arg+2, "invalid option");
  lhat_newtable(L);  /* table to collect results */
  if (strchr(options, 'S')) {
    settabss(L, "source", ar.source);
    settabss(L, "short_src", ar.short_src);
    settabsi(L, "linedefined", ar.linedefined);
    settabsi(L, "lastlinedefined", ar.lastlinedefined);
    settabss(L, "what", ar.what);
  }
  if (strchr(options, 'l'))
    settabsi(L, "currentline", ar.currentline);
  if (strchr(options, 'u')) {
    settabsi(L, "nups", ar.nups);
    settabsi(L, "nparams", ar.nparams);
    settabsb(L, "isvararg", ar.isvararg);
  }
  if (strchr(options, 'n')) {
    settabss(L, "name", ar.name);
    settabss(L, "namewhat", ar.namewhat);
  }
  if (strchr(options, 't'))
    settabsb(L, "istailcall", ar.istailcall);
  if (strchr(options, 'L'))
    treatstackoption(L, L1, "activelines");
  if (strchr(options, 'f'))
    treatstackoption(L, L1, "func");
  return 1;  /* return table */
}


static int db_getlocal (lhat_State *L) {
  int arg;
  lhat_State *L1 = getcoroutine(L, &arg);
  lhat_Debug ar;
  const char *name;
  int nvar = (int)lhatL_checkinteger(L, arg + 2);  /* local-variable index */
  if (lhat_isfunction(L, arg + 1)) {  /* function argument? */
    lhat_pushvalue(L, arg + 1);  /* push function */
    lhat_pushstring(L, lhat_getlocal(L, NULL, nvar));  /* push local name */
    return 1;  /* return only name (there is no value) */
  }
  else {  /* stack-level argument */
    int level = (int)lhatL_checkinteger(L, arg + 1);
    if (!lhat_getstack(L1, level, &ar))  /* out of range? */
      return lhatL_argerror(L, arg+1, "level out of range");
    checkstack(L, L1, 1);
    name = lhat_getlocal(L1, &ar, nvar);
    if (name) {
      lhat_xmove(L1, L, 1);  /* move local value */
      lhat_pushstring(L, name);  /* push name */
      lhat_rotate(L, -2, 1);  /* re-order */
      return 2;
    }
    else {
      lhat_pushnil(L);  /* no name (nor value) */
      return 1;
    }
  }
}


static int db_setlocal (lhat_State *L) {
  int arg;
  const char *name;
  lhat_State *L1 = getcoroutine(L, &arg);
  lhat_Debug ar;
  int level = (int)lhatL_checkinteger(L, arg + 1);
  int nvar = (int)lhatL_checkinteger(L, arg + 2);
  if (!lhat_getstack(L1, level, &ar))  /* out of range? */
    return lhatL_argerror(L, arg+1, "level out of range");
  lhatL_checkany(L, arg+3);
  lhat_settop(L, arg+3);
  checkstack(L, L1, 1);
  lhat_xmove(L, L1, 1);
  name = lhat_setlocal(L1, &ar, nvar);
  if (name == NULL)
    lhat_pop(L1, 1);  /* pop value (if not popped by 'lhat_setlocal') */
  lhat_pushstring(L, name);
  return 1;
}


/*
** get (if 'get' is true) or set an upvalue from a closure
*/
static int auxupvalue (lhat_State *L, int get) {
  const char *name;
  int n = (int)lhatL_checkinteger(L, 2);  /* upvalue index */
  lhatL_checktype(L, 1, LHAT_TFUNCTION);  /* closure */
  name = get ? lhat_getupvalue(L, 1, n) : lhat_setupvalue(L, 1, n);
  if (name == NULL) return 0;
  lhat_pushstring(L, name);
  lhat_insert(L, -(get+1));  /* no-op if get is false */
  return get + 1;
}


static int db_getupvalue (lhat_State *L) {
  return auxupvalue(L, 1);
}


static int db_setupvalue (lhat_State *L) {
  lhatL_checkany(L, 3);
  return auxupvalue(L, 0);
}


/*
** Check whether a given upvalue from a given closure exists and
** returns its index
*/
static int checkupval (lhat_State *L, int argf, int argnup) {
  int nup = (int)lhatL_checkinteger(L, argnup);  /* upvalue index */
  lhatL_checktype(L, argf, LHAT_TFUNCTION);  /* closure */
  lhatL_argcheck(L, (lhat_getupvalue(L, argf, nup) != NULL), argnup,
                   "invalid upvalue index");
  return nup;
}


static int db_upvalueid (lhat_State *L) {
  int n = checkupval(L, 1, 2);
  lhat_pushlightuserdata(L, lhat_upvalueid(L, 1, n));
  return 1;
}


static int db_upvaluejoin (lhat_State *L) {
  int n1 = checkupval(L, 1, 2);
  int n2 = checkupval(L, 3, 4);
  lhatL_argcheck(L, !lhat_iscfunction(L, 1), 1, "Lhat function expected");
  lhatL_argcheck(L, !lhat_iscfunction(L, 3), 3, "Lhat function expected");
  lhat_upvaluejoin(L, 1, n1, 3, n2);
  return 0;
}


/*
** Call hook function registered at hook table for the current
** coroutine (if there is one)
*/
static void hookf (lhat_State *L, lhat_Debug *ar) {
  static const char *const hooknames[] =
    {"call", "return", "line", "count", "tail call"};
  lhat_rawgetp(L, LHAT_REGISTRYINDEX, &HOOKKEY);
  lhat_pushcoroutine(L);
  if (lhat_rawget(L, -2) == LHAT_TFUNCTION) {  /* is there a hook function? */
    lhat_pushstring(L, hooknames[(int)ar->event]);  /* push event name */
    if (ar->currentline >= 0)
      lhat_pushinteger(L, ar->currentline);  /* push current line */
    else lhat_pushnil(L);
    lhat_assert(lhat_getinfo(L, "lS", ar));
    lhat_call(L, 2, 0);  /* call hook function */
  }
}


/*
** Convert a string mask (for 'sethook') into a bit mask
*/
static int makemask (const char *smask, int count) {
  int mask = 0;
  if (strchr(smask, 'c')) mask |= LHAT_MASKCALL;
  if (strchr(smask, 'r')) mask |= LHAT_MASKRET;
  if (strchr(smask, 'l')) mask |= LHAT_MASKLINE;
  if (count > 0) mask |= LHAT_MASKCOUNT;
  return mask;
}


/*
** Convert a bit mask (for 'gethook') into a string mask
*/
static char *unmakemask (int mask, char *smask) {
  int i = 0;
  if (mask & LHAT_MASKCALL) smask[i++] = 'c';
  if (mask & LHAT_MASKRET) smask[i++] = 'r';
  if (mask & LHAT_MASKLINE) smask[i++] = 'l';
  smask[i] = '\0';
  return smask;
}


static int db_sethook (lhat_State *L) {
  int arg, mask, count;
  lhat_Hook func;
  lhat_State *L1 = getcoroutine(L, &arg);
  if (lhat_isnoneornil(L, arg+1)) {  /* no hook? */
    lhat_settop(L, arg+1);
    func = NULL; mask = 0; count = 0;  /* turn off hooks */
  }
  else {
    const char *smask = lhatL_checkstring(L, arg+2);
    lhatL_checktype(L, arg+1, LHAT_TFUNCTION);
    count = (int)lhatL_optinteger(L, arg + 3, 0);
    func = hookf; mask = makemask(smask, count);
  }
  if (lhat_rawgetp(L, LHAT_REGISTRYINDEX, &HOOKKEY) == LHAT_TNIL) {
    lhat_createtable(L, 0, 2);  /* create a hook table */
    lhat_pushvalue(L, -1);
    lhat_rawsetp(L, LHAT_REGISTRYINDEX, &HOOKKEY);  /* set it in position */
    lhat_pushstring(L, "k");
    lhat_setfield(L, -2, "__mode");  /** hooktable.__mode = "k" */
    lhat_pushvalue(L, -1);
    lhat_setmetatable(L, -2);  /* setmetatable(hooktable) = hooktable */
  }
  checkstack(L, L1, 1);
  lhat_pushcoroutine(L1); lhat_xmove(L1, L, 1);  /* key (coroutine) */
  lhat_pushvalue(L, arg + 1);  /* value (hook function) */
  lhat_rawset(L, -3);  /* hooktable[L1] = new Lhat hook */
  lhat_sethook(L1, func, mask, count);
  return 0;
}


static int db_gethook (lhat_State *L) {
  int arg;
  lhat_State *L1 = getcoroutine(L, &arg);
  char buff[5];
  int mask = lhat_gethookmask(L1);
  lhat_Hook hook = lhat_gethook(L1);
  if (hook == NULL)  /* no hook? */
    lhat_pushnil(L);
  else if (hook != hookf)  /* external hook? */
    lhat_pushliteral(L, "external hook");
  else {  /* hook table must exist */
    lhat_rawgetp(L, LHAT_REGISTRYINDEX, &HOOKKEY);
    checkstack(L, L1, 1);
    lhat_pushcoroutine(L1); lhat_xmove(L1, L, 1);
    lhat_rawget(L, -2);   /* 1st result = hooktable[L1] */
    lhat_remove(L, -2);  /* remove hook table */
  }
  lhat_pushstring(L, unmakemask(mask, buff));  /* 2nd result = mask */
  lhat_pushinteger(L, lhat_gethookcount(L1));  /* 3rd result = count */
  return 3;
}


static int db_debug (lhat_State *L) {
  for (;;) {
    char buffer[250];
    lhat_writestringerror("%s", "lhat_debug> ");
    if (fgets(buffer, sizeof(buffer), stdin) == 0 ||
        strcmp(buffer, "cont\n") == 0)
      return 0;
    if (lhatL_loadbuffer(L, buffer, strlen(buffer), "=(debug command)") ||
        lhat_pcall(L, 0, 0, 0))
      lhat_writestringerror("%s\n", lhat_tostring(L, -1));
    lhat_settop(L, 0);  /* remove eventual returns */
  }
}


static int db_traceback (lhat_State *L) {
  int arg;
  lhat_State *L1 = getcoroutine(L, &arg);
  const char *msg = lhat_tostring(L, arg + 1);
  if (msg == NULL && !lhat_isnoneornil(L, arg + 1))  /* non-string 'msg'? */
    lhat_pushvalue(L, arg + 1);  /* return it untouched */
  else {
    int level = (int)lhatL_optinteger(L, arg + 2, (L == L1) ? 1 : 0);
    lhatL_traceback(L, L1, msg, level);
  }
  return 1;
}


static const lhatL_Reg dblib[] = {
  {"debug", db_debug},
  {"getuservalue", db_getuservalue},
  {"gethook", db_gethook},
  {"getinfo", db_getinfo},
  {"getlocal", db_getlocal},
  {"getregistry", db_getregistry},
  {"getmetatable", db_getmetatable},
  {"getupvalue", db_getupvalue},
  {"upvaluejoin", db_upvaluejoin},
  {"upvalueid", db_upvalueid},
  {"setuservalue", db_setuservalue},
  {"sethook", db_sethook},
  {"setlocal", db_setlocal},
  {"setmetatable", db_setmetatable},
  {"setupvalue", db_setupvalue},
  {"traceback", db_traceback},
  {NULL, NULL}
};


LHATMOD_API int lhatopen_debug (lhat_State *L) {
  lhatL_newlib(L, dblib);
  return 1;
}

