//
// $Id: lbaselib.c,v 1.314 2016/09/05 19:06:34 roberto Exp $
// Basic library
// See Copyright Notice in lhat.h
//

#define lbaselib_c
#define LHAT_LIB

#include "lprefix.h"


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"


static int lhatB_print (lhat_State *L) {
  int n = lhat_gettop(L);  // number of arguments
  int i;
  lhat_getglobal(L, "tostring");
  for (i=1; i<=n; i++) {
    const char *s;
    size_t l;
    lhat_pushvalue(L, -1);  // function to be called
    lhat_pushvalue(L, i);   // value to print
    lhat_call(L, 1, 1);
    s = lhat_tolstring(L, -1, &l);  // get result
    if (s == NULL)
      return lhatL_error(L, "'tostring' must return a string to 'print'");
    if (i>1) lhat_writestring("\t", 1);
    lhat_writestring(s, l);
    lhat_pop(L, 1);  // pop result
  }
  lhat_writeline();
  return 0;
}


#define SPACECHARS	" \f\n\r\t\v"

static const char *b_str2int (const char *s, int base, lhat_Integer *pn) {
  lhat_Unsigned n = 0;
  int neg = 0;
  s += strspn(s, SPACECHARS);  // skip initial spaces
  if (*s == '-') { s++; neg = 1; }  // handle signal
  else if (*s == '+') s++;
  if (!isalnum((unsigned char)*s))  // no digit?
    return NULL;
  do {
    int digit = (isdigit((unsigned char)*s)) ? *s - '0'
                   : (toupper((unsigned char)*s) - 'A') + 10;
    if (digit >= base) return NULL;  // invalid numeral
    n = n * base + digit;
    s++;
  } while (isalnum((unsigned char)*s));
  s += strspn(s, SPACECHARS);  // skip trailing spaces
  *pn = (lhat_Integer)((neg) ? (0u - n) : n);
  return s;
}


static int lhatB_tonumber (lhat_State *L) {
  if (lhat_isnoneornil(L, 2)) {  // standard conversion?
    lhatL_checkany(L, 1);
    if (lhat_type(L, 1) == LHAT_TNUMBER) {  // already a number?
      lhat_settop(L, 1);  // yes; return it
      return 1;
    }
    else {
      size_t l;
      const char *s = lhat_tolstring(L, 1, &l);
      if (s != NULL && lhat_stringtonumber(L, s) == l + 1)
        return 1;  // successful conversion to number
      // else not a number
    }
  }
  else {
    size_t l;
    const char *s;
    lhat_Integer n = 0;  // to avoid warnings
    lhat_Integer base = lhatL_checkinteger(L, 2);
    lhatL_checktype(L, 1, LHAT_TSTRING);  // no numbers as strings
    s = lhat_tolstring(L, 1, &l);
    lhatL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");
    if (b_str2int(s, (int)base, &n) == s + l) {
      lhat_pushinteger(L, n);
      return 1;
    }  // else not a number
  }  // else not a number
  lhat_pushnil(L);  // not a number
  return 1;
}


static int lhatB_error (lhat_State *L) {
  int level = (int)lhatL_optinteger(L, 2, 1);
  lhat_settop(L, 1);
  if (lhat_type(L, 1) == LHAT_TSTRING && level > 0) {
    lhatL_where(L, level);   // add extra information
    lhat_pushvalue(L, 1);
    lhat_concat(L, 2);
  }
  return lhat_error(L);
}


static int lhatB_getmetatable (lhat_State *L) {
  lhatL_checkany(L, 1);
  if (!lhat_getmetatable(L, 1)) {
    lhat_pushnil(L);
    return 1;  // no metatable
  }
  lhatL_getmetafield(L, 1, "__metatable");
  return 1;  // returns either __metatable field (if present) or metatable
}


static int lhatB_setmetatable (lhat_State *L) {
  int t = lhat_type(L, 2);
  lhatL_checktype(L, 1, LHAT_TTABLE);
  lhatL_argcheck(L, t == LHAT_TNIL || t == LHAT_TTABLE, 2,
                    "nil or table expected");
  if (lhatL_getmetafield(L, 1, "__metatable") != LHAT_TNIL)
    return lhatL_error(L, "cannot change a protected metatable");
  lhat_settop(L, 2);
  lhat_setmetatable(L, 1);
  return 1;
}


static int lhatB_rawequal (lhat_State *L) {
  lhatL_checkany(L, 1);
  lhatL_checkany(L, 2);
  lhat_pushboolean(L, lhat_rawequal(L, 1, 2));
  return 1;
}


static int lhatB_rawlen (lhat_State *L) {
  int t = lhat_type(L, 1);
  lhatL_argcheck(L, t == LHAT_TTABLE || t == LHAT_TSTRING, 1,
                   "table or string expected");
  lhat_pushinteger(L, lhat_rawlen(L, 1));
  return 1;
}


static int lhatB_rawget (lhat_State *L) {
  lhatL_checktype(L, 1, LHAT_TTABLE);
  lhatL_checkany(L, 2);
  lhat_settop(L, 2);
  lhat_rawget(L, 1);
  return 1;
}

static int lhatB_rawset (lhat_State *L) {
  lhatL_checktype(L, 1, LHAT_TTABLE);
  lhatL_checkany(L, 2);
  lhatL_checkany(L, 3);
  lhat_settop(L, 3);
  lhat_rawset(L, 1);
  return 1;
}


static int lhatB_collectgarbage (lhat_State *L) {
  static const char *const opts[] = {"stop", "restart", "collect",
    "count", "step", "setpause", "setstepmul",
    "isrunning", NULL};
  static const int optsnum[] = {LHAT_GCSTOP, LHAT_GCRESTART, LHAT_GCCOLLECT,
    LHAT_GCCOUNT, LHAT_GCSTEP, LHAT_GCSETPAUSE, LHAT_GCSETSTEPMUL,
    LHAT_GCISRUNNING};
  int o = optsnum[lhatL_checkoption(L, 1, "collect", opts)];
  int ex = (int)lhatL_optinteger(L, 2, 0);
  int res = lhat_gc(L, o, ex);
  switch (o) {
    case LHAT_GCCOUNT: {
      int b = lhat_gc(L, LHAT_GCCOUNTB, 0);
      lhat_pushnumber(L, (lhat_Number)res + ((lhat_Number)b/1024));
      return 1;
    }
    case LHAT_GCSTEP: case LHAT_GCISRUNNING: {
      lhat_pushboolean(L, res);
      return 1;
    }
    default: {
      lhat_pushinteger(L, res);
      return 1;
    }
  }
}


static int lhatB_type (lhat_State *L) {
  int t = lhat_type(L, 1);
  lhatL_argcheck(L, t != LHAT_TNONE, 1, "value expected");
  lhat_pushstring(L, lhat_typename(L, t));
  return 1;
}


static int pairsmeta (lhat_State *L, const char *method, int iszero,
                      lhat_CFunction iter) {
  lhatL_checkany(L, 1);
  if (lhatL_getmetafield(L, 1, method) == LHAT_TNIL) {  // no metamethod?
    lhat_pushcfunction(L, iter);  // will return generator,
    lhat_pushvalue(L, 1);  // state,
    if (iszero) lhat_pushinteger(L, 0);  // and initial value
    else lhat_pushnil(L);
  }
  else {
    lhat_pushvalue(L, 1);  // argument 'self' to metamethod
    lhat_call(L, 1, 3);  // get 3 values from metamethod
  }
  return 3;
}


static int lhatB_next (lhat_State *L) {
  lhatL_checktype(L, 1, LHAT_TTABLE);
  lhat_settop(L, 2);  // create a 2nd argument if there isn't one
  if (lhat_next(L, 1))
    return 2;
  else {
    lhat_pushnil(L);
    return 1;
  }
}


static int lhatB_pairs (lhat_State *L) {
  return pairsmeta(L, "__pairs", 0, lhatB_next);
}


//
// Traversal function for 'ipairs'
//
static int ipairsaux (lhat_State *L) {
  lhat_Integer i = lhatL_checkinteger(L, 2) + 1;
  lhat_pushinteger(L, i);
  return (lhat_geti(L, 1, i) == LHAT_TNIL) ? 1 : 2;
}


//
// 'ipairs' function. Returns 'ipairsaux', given "table", 0.
// (The given "table" may not be a table.)
//
static int lhatB_ipairs (lhat_State *L) {
#if defined(LHAT_COMPAT_IPAIRS)
  return pairsmeta(L, "__ipairs", 1, ipairsaux);
#else
  lhatL_checkany(L, 1);
  lhat_pushcfunction(L, ipairsaux);  // iteration function
  lhat_pushvalue(L, 1);  // state
  lhat_pushinteger(L, 0);  // initial value
  return 3;
#endif
}


static int load_aux (lhat_State *L, int status, int envidx) {
  if (status == LHAT_OK) {
    if (envidx != 0) {  // 'env' parameter?
      lhat_pushvalue(L, envidx);  // environment for loaded function
      if (!lhat_setupvalue(L, -2, 1))  // set it as 1st upvalues
        lhat_pop(L, 1);  // remove 'env' if not used by previous call
    }
    return 1;
  }
  else {  // error (message is on top of the stack)
    lhat_pushnil(L);
    lhat_insert(L, -2);  // put before error message
    return 2;  // return nil plus error message
  }
}


static int lhatB_loadfile (lhat_State *L) {
  const char *fname = lhatL_optstring(L, 1, NULL);
  const char *mode = lhatL_optstring(L, 2, NULL);
  int env = (!lhat_isnone(L, 3) ? 3 : 0);  // 'env' index or 0 if no 'env'
  int status = lhatL_loadfilex(L, fname, mode);
  return load_aux(L, status, env);
}


//
// {======================================================
// Generic Read function
// =======================================================
//


//
// reserved slot, above all arguments, to hold a copy of the returned
// string to avoid it being collected while parsed. 'load' has four
// optional arguments (chunk, source name, mode, and environment).
//
#define RESERVEDSLOT	5


//
// Reader for generic 'load' function: 'lhat_load' uses the
// stack for internal stuff, so the reader cannot change the
// stack top. Instead, it keeps its resulting string in a
// reserved slot inside the stack.
//
static const char *generic_reader (lhat_State *L, void *ud, size_t *size) {
  (void)(ud);  // not used
  lhatL_checkstack(L, 2, "too many nested functions");
  lhat_pushvalue(L, 1);  // get function
  lhat_call(L, 0, 1);  // call it
  if (lhat_isnil(L, -1)) {
    lhat_pop(L, 1);  // pop result
    *size = 0;
    return NULL;
  }
  else if (!lhat_isstring(L, -1))
    lhatL_error(L, "reader function must return a string");
  lhat_replace(L, RESERVEDSLOT);  // save string in reserved slot
  return lhat_tolstring(L, RESERVEDSLOT, size);
}


static int lhatB_load (lhat_State *L) {
  int status;
  size_t l;
  const char *s = lhat_tolstring(L, 1, &l);
  const char *mode = lhatL_optstring(L, 3, "bt");
  int env = (!lhat_isnone(L, 4) ? 4 : 0);  // 'env' index or 0 if no 'env'
  if (s != NULL) {  // loading a string?
    const char *chunkname = lhatL_optstring(L, 2, s);
    status = lhatL_loadbufferx(L, s, l, chunkname, mode);
  }
  else {  // loading from a reader function
    const char *chunkname = lhatL_optstring(L, 2, "=(load)");
    lhatL_checktype(L, 1, LHAT_TFUNCTION);
    lhat_settop(L, RESERVEDSLOT);  // create reserved slot
    status = lhat_load(L, generic_reader, NULL, chunkname, mode);
  }
  return load_aux(L, status, env);
}

// }======================================================


static int dofilecont (lhat_State *L, int d1, lhat_KContext d2) {
  (void)d1;  (void)d2;  // only to match 'lhat_Kfunction' prototype
  return lhat_gettop(L) - 1;
}


static int lhatB_dofile (lhat_State *L) {
  const char *fname = lhatL_optstring(L, 1, NULL);
  lhat_settop(L, 1);
  if (lhatL_loadfile(L, fname) != LHAT_OK)
    return lhat_error(L);
  lhat_callk(L, 0, LHAT_MULTRET, 0, dofilecont);
  return dofilecont(L, 0, 0);
}


static int lhatB_assert (lhat_State *L) {
  if (lhat_toboolean(L, 1))  // condition is true?
    return lhat_gettop(L);  // return all arguments
  else {  // error
    lhatL_checkany(L, 1);  // there must be a condition
    lhat_remove(L, 1);  // remove it
    lhat_pushliteral(L, "assertion failed!");  // default message
    lhat_settop(L, 1);  // leave only message (default if no other one)
    return lhatB_error(L);  // call 'error'
  }
}


static int lhatB_select (lhat_State *L) {
  int n = lhat_gettop(L);
  if (lhat_type(L, 1) == LHAT_TSTRING && *lhat_tostring(L, 1) == '#') {
    lhat_pushinteger(L, n-1);
    return 1;
  }
  else {
    lhat_Integer i = lhatL_checkinteger(L, 1);
    if (i < 0) i = n + i;
    else if (i > n) i = n;
    lhatL_argcheck(L, 1 <= i, 1, "index out of range");
    return n - (int)i;
  }
}


//
// Continuation function for 'pcall' and 'xpcall'. Both functions
// already pushed a 'true' before doing the call, so in case of success
// 'finishpcall' only has to return everything in the stack minus
// 'extra' values (where 'extra' is exactly the number of items to be
// ignored).
//
static int finishpcall (lhat_State *L, int status, lhat_KContext extra) {
  if (status != LHAT_OK && status != LHAT_YIELD) {  // error?
    lhat_pushboolean(L, 0);  // first result (false)
    lhat_pushvalue(L, -2);  // error message
    return 2;  // return false, msg
  }
  else
    return lhat_gettop(L) - (int)extra;  // return all results
}


static int lhatB_pcall (lhat_State *L) {
  int status;
  lhatL_checkany(L, 1);
  lhat_pushboolean(L, 1);  // first result if no errors
  lhat_insert(L, 1);  // put it in place
  status = lhat_pcallk(L, lhat_gettop(L) - 2, LHAT_MULTRET, 0, 0, finishpcall);
  return finishpcall(L, status, 0);
}


//
// Do a protected call with error handling. After 'lhat_rotate', the
// stack will have <f, err, true, f, [args...]>; so, the function passes
// 2 to 'finishpcall' to skip the 2 first values when returning results.
//
static int lhatB_xpcall (lhat_State *L) {
  int status;
  int n = lhat_gettop(L);
  lhatL_checktype(L, 2, LHAT_TFUNCTION);  // check error function
  lhat_pushboolean(L, 1);  // first result
  lhat_pushvalue(L, 1);  // function
  lhat_rotate(L, 3, 2);  // move them below function's arguments
  status = lhat_pcallk(L, n - 2, LHAT_MULTRET, 2, 2, finishpcall);
  return finishpcall(L, status, 2);
}


static int lhatB_tostring (lhat_State *L) {
  lhatL_checkany(L, 1);
  lhatL_tolstring(L, 1, NULL);
  return 1;
}


static const lhatL_Reg base_funcs[] = {
  {"assert", lhatB_assert},
  {"collectgarbage", lhatB_collectgarbage},
  {"dofile", lhatB_dofile},
  {"error", lhatB_error},
  {"getmetatable", lhatB_getmetatable},
  {"ipairs", lhatB_ipairs},
  {"loadfile", lhatB_loadfile},
  {"load", lhatB_load},
#if defined(LHAT_COMPAT_LOADSTRING)
  {"loadstring", lhatB_load},
#endif
  {"next", lhatB_next},
  {"pairs", lhatB_pairs},
  {"pcall", lhatB_pcall},
  {"print", lhatB_print},
  {"rawequal", lhatB_rawequal},
  {"rawlen", lhatB_rawlen},
  {"rawget", lhatB_rawget},
  {"rawset", lhatB_rawset},
  {"select", lhatB_select},
  {"setmetatable", lhatB_setmetatable},
  {"tonumber", lhatB_tonumber},
  {"tostring", lhatB_tostring},
  {"type", lhatB_type},
  {"xpcall", lhatB_xpcall},
  // placeholders
  {"_G", NULL},
  {"_VERSION", NULL},
  {NULL, NULL}
};


LHATMOD_API int lhatopen_base (lhat_State *L) {
  // open lib into global table
  lhat_pushglobaltable(L);
  lhatL_setfuncs(L, base_funcs, 0);
  // set global _G
  lhat_pushvalue(L, -1);
  lhat_setfield(L, -2, "_G");
  // set global _VERSION
  lhat_pushliteral(L, LHAT_VERSION);
  lhat_setfield(L, -2, "_VERSION");
  return 1;
}

