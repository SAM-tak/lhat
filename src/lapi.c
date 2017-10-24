//
// $Id: lapi.c,v 2.259 2016/02/29 14:27:14 roberto Exp $
// Lhat API
// See Copyright Notice in lhat.h
//

#define lapi_c
#define LHAT_CORE

#include "lprefix.h"


#include <stdarg.h>
#include <string.h>

#include "lhat.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"


// value at a non-valid index
#define NONVALIDVALUE		cast(TValue *, lhatO_nilobject)

// corresponding test
#define isvalid(o)	((o) != lhatO_nilobject)

// test for pseudo index
#define ispseudo(i)		((i) <= LHAT_REGISTRYINDEX)

// test for upvalues
#define isupvalue(i)		((i) < LHAT_REGISTRYINDEX)

// test for valid but not pseudo index
#define isstackindex(i, o)	(isvalid(o) && !ispseudo(i))

#define api_checkvalidindex(l,o)  api_check(l, isvalid(o), "invalid index")

#define api_checkstackindex(l, i, o)  \
	api_check(l, isstackindex(i, o), "index not in the stack")


static TValue *index2addr (lhat_State *L, int idx) {
  CallInfo *ci = L->ci;
  if (idx > 0) {
    TValue *o = ci->func + idx;
    api_check(L, idx <= ci->top - (ci->func + 1), "unacceptable index");
    if (o >= L->top) return NONVALIDVALUE;
    else return o;
  }
  else if (!ispseudo(idx)) {  // negative index
    api_check(L, idx != 0 && -idx <= L->top - (ci->func + 1), "invalid index");
    return L->top + idx;
  }
  else if (idx == LHAT_REGISTRYINDEX)
    return &G(L)->l_registry;
  else {  // upvalues
    idx = LHAT_REGISTRYINDEX - idx;
    api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
    if (ttislcf(ci->func))  // light C function?
      return NONVALIDVALUE;  // it has no upvalues
    else {
      CClosure *func = clCvalue(ci->func);
      return (idx <= func->nupvalues) ? &func->upvalues[idx-1] : NONVALIDVALUE;
    }
  }
}


//
// to be called by 'lhat_checkstack' in protected mode, to grow stack
// capturing memory errors
//
static void growstack (lhat_State *L, void *ud) {
  int size = *(int *)ud;
  lhatD_growstack(L, size);
}


LHAT_API int lhat_checkstack (lhat_State *L, int n) {
  int res;
  CallInfo *ci = L->ci;
  lhat_lock(L);
  api_check(L, n >= 0, "negative 'n'");
  if (L->stack_last - L->top > n)  // stack large enough?
    res = 1;  // yes; check is OK
  else {  // no; need to grow stack
    int inuse = cast_int(L->top - L->stack) + EXTRA_STACK;
    if (inuse > LHATI_MAXSTACK - n)  // can grow without overflow?
      res = 0;  // no
    else  // try to grow stack
      res = (lhatD_rawrunprotected(L, &growstack, &n) == LHAT_OK);
  }
  if (res && ci->top < L->top + n)
    ci->top = L->top + n;  // adjust frame top
  lhat_unlock(L);
  return res;
}


LHAT_API void lhat_xmove (lhat_State *from, lhat_State *to, int n) {
  int i;
  if (from == to) return;
  lhat_lock(to);
  api_checknelems(from, n);
  api_check(from, G(from) == G(to), "moving among independent states");
  api_check(from, to->ci->top - to->top >= n, "stack overflow");
  from->top -= n;
  for (i = 0; i < n; i++) {
    setobj2s(to, to->top, from->top + i);
    to->top++;  // stack already checked by previous 'api_check'
  }
  lhat_unlock(to);
}


LHAT_API lhat_CFunction lhat_atpanic (lhat_State *L, lhat_CFunction panicf) {
  lhat_CFunction old;
  lhat_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lhat_unlock(L);
  return old;
}


LHAT_API const lhat_Number *lhat_version (lhat_State *L) {
  static const lhat_Number version = LHAT_VERSION_NUM;
  if (L == NULL) return &version;
  else return G(L)->version;
}



//
// basic stack manipulation
//


//
// convert an acceptable stack index into an absolute index
//
LHAT_API int lhat_absindex (lhat_State *L, int idx) {
  return (idx > 0 || ispseudo(idx))
         ? idx
         : cast_int(L->top - L->ci->func) + idx;
}


LHAT_API int lhat_gettop (lhat_State *L) {
  return cast_int(L->top - (L->ci->func + 1));
}


LHAT_API void lhat_settop (lhat_State *L, int idx) {
  StkId func = L->ci->func;
  lhat_lock(L);
  if (idx >= 0) {
    api_check(L, idx <= L->stack_last - (func + 1), "new top too large");
    while (L->top < (func + 1) + idx)
      setnilvalue(L->top++);
    L->top = (func + 1) + idx;
  }
  else {
    api_check(L, -(idx+1) <= (L->top - (func + 1)), "invalid new top");
    L->top += idx+1;  // 'subtract' index (index is negative)
  }
  lhat_unlock(L);
}


//
// Reverse the stack segment from 'from' to 'to'
// (auxiliary to 'lhat_rotate')
//
static void reverse (lhat_State *L, StkId from, StkId to) {
  for (; from < to; from++, to--) {
    TValue temp;
    setobj(L, &temp, from);
    setobjs2s(L, from, to);
    setobj2s(L, to, &temp);
  }
}


//
// Let x = AB, where A is a prefix of length 'n'. Then,
// rotate x n == BA. But BA == (A^r . B^r)^r.
//
LHAT_API void lhat_rotate (lhat_State *L, int idx, int n) {
  StkId p, t, m;
  lhat_lock(L);
  t = L->top - 1;  // end of stack segment being rotated
  p = index2addr(L, idx);  // start of segment
  api_checkstackindex(L, idx, p);
  api_check(L, (n >= 0 ? n : -n) <= (t - p + 1), "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1);  // end of prefix
  reverse(L, p, m);  // reverse the prefix with length 'n'
  reverse(L, m + 1, t);  // reverse the suffix
  reverse(L, p, t);  // reverse the entire segment
  lhat_unlock(L);
}


LHAT_API void lhat_copy (lhat_State *L, int fromidx, int toidx) {
  TValue *fr, *to;
  lhat_lock(L);
  fr = index2addr(L, fromidx);
  to = index2addr(L, toidx);
  api_checkvalidindex(L, to);
  setobj(L, to, fr);
  if (isupvalue(toidx))  // function upvalues?
    lhatC_barrier(L, clCvalue(L->ci->func), fr);
  // LHAT_REGISTRYINDEX does not need gc barrier
  // (collector revisits it before finishing collection)
  lhat_unlock(L);
}


LHAT_API void lhat_pushvalue (lhat_State *L, int idx) {
  lhat_lock(L);
  setobj2s(L, L->top, index2addr(L, idx));
  api_incr_top(L);
  lhat_unlock(L);
}



//
// access functions (stack -> C)
//


LHAT_API int lhat_type (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (isvalid(o) ? ttnov(o) : LHAT_TNONE);
}


LHAT_API const char *lhat_typename (lhat_State *L, int t) {
  UNUSED(L);
  api_check(L, LHAT_TNONE <= t && t < LHAT_NUMTAGS, "invalid tag");
  return ttypename(t);
}


LHAT_API int lhat_iscfunction (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (ttislcf(o) || (ttisCclosure(o)));
}


LHAT_API int lhat_isinteger (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return ttisinteger(o);
}


LHAT_API int lhat_isnumber (lhat_State *L, int idx) {
  lhat_Number n;
  const TValue *o = index2addr(L, idx);
  return tonumber(o, &n);
}


LHAT_API int lhat_isstring (lhat_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisstring(o) || cvt2str(o));
}


LHAT_API int lhat_isuserdata (lhat_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisfulluserdata(o) || ttislightuserdata(o));
}


LHAT_API int lhat_rawequal (lhat_State *L, int index1, int index2) {
  StkId o1 = index2addr(L, index1);
  StkId o2 = index2addr(L, index2);
  return (isvalid(o1) && isvalid(o2)) ? lhatV_rawequalobj(o1, o2) : 0;
}


LHAT_API void lhat_arith (lhat_State *L, int op) {
  lhat_lock(L);
  if (op != LHAT_OPUNM && op != LHAT_OPBNOT)
    api_checknelems(L, 2);  // all other operations expect two operands
  else {  // for unary operations, add fake 2nd operand
    api_checknelems(L, 1);
    setobjs2s(L, L->top, L->top - 1);
    api_incr_top(L);
  }
  // first operand at top - 2, second at top - 1; result go to top - 2
  lhatO_arith(L, op, L->top - 2, L->top - 1, L->top - 2);
  L->top--;  // remove second operand
  lhat_unlock(L);
}


LHAT_API int lhat_compare (lhat_State *L, int index1, int index2, int op) {
  StkId o1, o2;
  int i = 0;
  lhat_lock(L);  // may call tag method
  o1 = index2addr(L, index1);
  o2 = index2addr(L, index2);
  if (isvalid(o1) && isvalid(o2)) {
    switch (op) {
      case LHAT_OPEQ: i = lhatV_equalobj(L, o1, o2); break;
      case LHAT_OPLT: i = lhatV_lessthan(L, o1, o2); break;
      case LHAT_OPLE: i = lhatV_lessequal(L, o1, o2); break;
      default: api_check(L, 0, "invalid option");
    }
  }
  lhat_unlock(L);
  return i;
}


LHAT_API size_t lhat_stringtonumber (lhat_State *L, const char *s) {
  size_t sz = lhatO_str2num(s, L->top);
  if (sz != 0)
    api_incr_top(L);
  return sz;
}


LHAT_API lhat_Number lhat_tonumberx (lhat_State *L, int idx, int *pisnum) {
  lhat_Number n;
  const TValue *o = index2addr(L, idx);
  int isnum = tonumber(o, &n);
  if (!isnum)
    n = 0;  // call to 'tonumber' may change 'n' even if it fails
  if (pisnum) *pisnum = isnum;
  return n;
}


LHAT_API lhat_Integer lhat_tointegerx (lhat_State *L, int idx, int *pisnum) {
  lhat_Integer res;
  const TValue *o = index2addr(L, idx);
  int isnum = tointeger(o, &res);
  if (!isnum)
    res = 0;  // call to 'tointeger' may change 'n' even if it fails
  if (pisnum) *pisnum = isnum;
  return res;
}


LHAT_API int lhat_toboolean (lhat_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return !l_isfalse(o);
}


LHAT_API const char *lhat_tolstring (lhat_State *L, int idx, size_t *len) {
  StkId o = index2addr(L, idx);
  if (!ttisstring(o)) {
    if (!cvt2str(o)) {  // not convertible?
      if (len != NULL) *len = 0;
      return NULL;
    }
    lhat_lock(L);  // 'lhatO_tostring' may create a new string
    lhatO_tostring(L, o);
    lhatC_checkGC(L);
    o = index2addr(L, idx);  // previous call may reallocate the stack
    lhat_unlock(L);
  }
  if (len != NULL)
    *len = vslen(o);
  return svalue(o);
}


LHAT_API size_t lhat_rawlen (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LHAT_TSHRSTR: return tsvalue(o)->shrlen;
    case LHAT_TLNGSTR: return tsvalue(o)->u.lnglen;
    case LHAT_TUSERDATA: return uvalue(o)->len;
    case LHAT_TTABLE: return lhatH_getn(hvalue(o));
    default: return 0;
  }
}


LHAT_API lhat_CFunction lhat_tocfunction (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  if (ttislcf(o)) return fvalue(o);
  else if (ttisCclosure(o))
    return clCvalue(o)->f;
  else return NULL;  // not a C function
}


LHAT_API void *lhat_touserdata (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttnov(o)) {
    case LHAT_TUSERDATA: return getudatamem(uvalue(o));
    case LHAT_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


LHAT_API lhat_State *lhat_tocoroutine (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (!ttiscoroutine(o)) ? NULL : thvalue(o);
}


LHAT_API const void *lhat_topointer (lhat_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LHAT_TTABLE: return hvalue(o);
    case LHAT_TLCL: return clLvalue(o);
    case LHAT_TCCL: return clCvalue(o);
    case LHAT_TLCF: return cast(void *, cast(size_t, fvalue(o)));
    case LHAT_TCOROUTINE: return thvalue(o);
    case LHAT_TUSERDATA: return getudatamem(uvalue(o));
    case LHAT_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}



//
// push functions (C -> stack)
//


LHAT_API void lhat_pushnil (lhat_State *L) {
  lhat_lock(L);
  setnilvalue(L->top);
  api_incr_top(L);
  lhat_unlock(L);
}


LHAT_API void lhat_pushnumber (lhat_State *L, lhat_Number n) {
  lhat_lock(L);
  setfltvalue(L->top, n);
  api_incr_top(L);
  lhat_unlock(L);
}


LHAT_API void lhat_pushinteger (lhat_State *L, lhat_Integer n) {
  lhat_lock(L);
  setivalue(L->top, n);
  api_incr_top(L);
  lhat_unlock(L);
}


//
// Pushes on the stack a string with given length. Avoid using 's' when
// 'len' == 0 (as 's' can be NULL in that case), due to later use of
// 'memcmp' and 'memcpy'.
//
LHAT_API const char *lhat_pushlstring (lhat_State *L, const char *s, size_t len) {
  TString *ts;
  lhat_lock(L);
  ts = (len == 0) ? lhatS_new(L, "") : lhatS_newlstr(L, s, len);
  setsvalue2s(L, L->top, ts);
  api_incr_top(L);
  lhatC_checkGC(L);
  lhat_unlock(L);
  return getstr(ts);
}


LHAT_API const char *lhat_pushstring (lhat_State *L, const char *s) {
  lhat_lock(L);
  if (s == NULL)
    setnilvalue(L->top);
  else {
    TString *ts;
    ts = lhatS_new(L, s);
    setsvalue2s(L, L->top, ts);
    s = getstr(ts);  // internal copy's address
  }
  api_incr_top(L);
  lhatC_checkGC(L);
  lhat_unlock(L);
  return s;
}


LHAT_API const char *lhat_pushvfstring (lhat_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lhat_lock(L);
  ret = lhatO_pushvfstring(L, fmt, argp);
  lhatC_checkGC(L);
  lhat_unlock(L);
  return ret;
}


LHAT_API const char *lhat_pushfstring (lhat_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lhat_lock(L);
  va_start(argp, fmt);
  ret = lhatO_pushvfstring(L, fmt, argp);
  va_end(argp);
  lhatC_checkGC(L);
  lhat_unlock(L);
  return ret;
}


LHAT_API void lhat_pushcclosure (lhat_State *L, lhat_CFunction fn, int n) {
  lhat_lock(L);
  if (n == 0) {
    setfvalue(L->top, fn);
  }
  else {
    CClosure *cl;
    api_checknelems(L, n);
    api_check(L, n <= MAXUPVAL, "upvalue index too large");
    cl = lhatF_newCclosure(L, n);
    cl->f = fn;
    L->top -= n;
    while (n--) {
      setobj2n(L, &cl->upvalues[n], L->top + n);
      // does not need barrier because closure is white
    }
    setclCvalue(L, L->top, cl);
  }
  api_incr_top(L);
  lhatC_checkGC(L);
  lhat_unlock(L);
}


LHAT_API void lhat_pushboolean (lhat_State *L, int b) {
  lhat_lock(L);
  setbvalue(L->top, (b != 0));  // ensure that true is 1
  api_incr_top(L);
  lhat_unlock(L);
}


LHAT_API void lhat_pushlightuserdata (lhat_State *L, void *p) {
  lhat_lock(L);
  setpvalue(L->top, p);
  api_incr_top(L);
  lhat_unlock(L);
}


LHAT_API int lhat_pushcoroutine (lhat_State *L) {
  lhat_lock(L);
  setthvalue(L, L->top, L);
  api_incr_top(L);
  lhat_unlock(L);
  return (G(L)->maincoroutine == L);
}



//
// get functions (Lhat -> stack)
//


static int auxgetstr (lhat_State *L, const TValue *t, const char *k) {
  const TValue *slot;
  TString *str = lhatS_new(L, k);
  if (lhatV_fastget(L, t, str, slot, lhatH_getstr)) {
    setobj2s(L, L->top, slot);
    api_incr_top(L);
  }
  else {
    setsvalue2s(L, L->top, str);
    api_incr_top(L);
    lhatV_finishget(L, t, L->top - 1, L->top - 1, slot);
  }
  lhat_unlock(L);
  return ttnov(L->top - 1);
}


LHAT_API int lhat_getglobal (lhat_State *L, const char *name) {
  Table *reg = hvalue(&G(L)->l_registry);
  lhat_lock(L);
  return auxgetstr(L, lhatH_getint(reg, LHAT_RIDX_GLOBALS), name);
}


LHAT_API int lhat_gettable (lhat_State *L, int idx) {
  StkId t;
  lhat_lock(L);
  t = index2addr(L, idx);
  lhatV_gettable(L, t, L->top - 1, L->top - 1);
  lhat_unlock(L);
  return ttnov(L->top - 1);
}


LHAT_API int lhat_getfield (lhat_State *L, int idx, const char *k) {
  lhat_lock(L);
  return auxgetstr(L, index2addr(L, idx), k);
}


LHAT_API int lhat_geti (lhat_State *L, int idx, lhat_Integer n) {
  StkId t;
  const TValue *slot;
  lhat_lock(L);
  t = index2addr(L, idx);
  if (lhatV_fastget(L, t, n, slot, lhatH_getint)) {
    setobj2s(L, L->top, slot);
    api_incr_top(L);
  }
  else {
    setivalue(L->top, n);
    api_incr_top(L);
    lhatV_finishget(L, t, L->top - 1, L->top - 1, slot);
  }
  lhat_unlock(L);
  return ttnov(L->top - 1);
}


LHAT_API int lhat_rawget (lhat_State *L, int idx) {
  StkId t;
  lhat_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top - 1, lhatH_get(hvalue(t), L->top - 1));
  lhat_unlock(L);
  return ttnov(L->top - 1);
}


LHAT_API int lhat_rawgeti (lhat_State *L, int idx, lhat_Integer n) {
  StkId t;
  lhat_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top, lhatH_getint(hvalue(t), n));
  api_incr_top(L);
  lhat_unlock(L);
  return ttnov(L->top - 1);
}


LHAT_API int lhat_rawgetp (lhat_State *L, int idx, const void *p) {
  StkId t;
  TValue k;
  lhat_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setpvalue(&k, cast(void *, p));
  setobj2s(L, L->top, lhatH_get(hvalue(t), &k));
  api_incr_top(L);
  lhat_unlock(L);
  return ttnov(L->top - 1);
}


LHAT_API void lhat_createtable (lhat_State *L, int narray, int nrec) {
  Table *t;
  lhat_lock(L);
  t = lhatH_new(L);
  sethvalue(L, L->top, t);
  api_incr_top(L);
  if (narray > 0 || nrec > 0)
    lhatH_resize(L, t, narray, nrec);
  lhatC_checkGC(L);
  lhat_unlock(L);
}


LHAT_API int lhat_getmetatable (lhat_State *L, int objindex) {
  const TValue *obj;
  Table *mt;
  int res = 0;
  lhat_lock(L);
  obj = index2addr(L, objindex);
  switch (ttnov(obj)) {
    case LHAT_TTABLE:
      mt = hvalue(obj)->metatable;
      break;
    case LHAT_TUSERDATA:
      mt = uvalue(obj)->metatable;
      break;
    default:
      mt = G(L)->mt[ttnov(obj)];
      break;
  }
  if (mt != NULL) {
    sethvalue(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lhat_unlock(L);
  return res;
}


LHAT_API int lhat_getuservalue (lhat_State *L, int idx) {
  StkId o;
  lhat_lock(L);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  getuservalue(L, uvalue(o), L->top);
  api_incr_top(L);
  lhat_unlock(L);
  return ttnov(L->top - 1);
}


//
// set functions (stack -> Lhat)
//

//
// t[k] = value at the top of the stack (where 'k' is a string)
//
static void auxsetstr (lhat_State *L, const TValue *t, const char *k) {
  const TValue *slot;
  TString *str = lhatS_new(L, k);
  api_checknelems(L, 1);
  if (lhatV_fastset(L, t, str, slot, lhatH_getstr, L->top - 1))
    L->top--;  // pop value
  else {
    setsvalue2s(L, L->top, str);  // push 'str' (to make it a TValue)
    api_incr_top(L);
    lhatV_finishset(L, t, L->top - 1, L->top - 2, slot);
    L->top -= 2;  // pop value and key
  }
  lhat_unlock(L);  // lock done by caller
}


LHAT_API void lhat_setglobal (lhat_State *L, const char *name) {
  Table *reg = hvalue(&G(L)->l_registry);
  lhat_lock(L);  // unlock done in 'auxsetstr'
  auxsetstr(L, lhatH_getint(reg, LHAT_RIDX_GLOBALS), name);
}


LHAT_API void lhat_settable (lhat_State *L, int idx) {
  StkId t;
  lhat_lock(L);
  api_checknelems(L, 2);
  t = index2addr(L, idx);
  lhatV_settable(L, t, L->top - 2, L->top - 1);
  L->top -= 2;  // pop index and value
  lhat_unlock(L);
}


LHAT_API void lhat_setfield (lhat_State *L, int idx, const char *k) {
  lhat_lock(L);  // unlock done in 'auxsetstr'
  auxsetstr(L, index2addr(L, idx), k);
}


LHAT_API void lhat_seti (lhat_State *L, int idx, lhat_Integer n) {
  StkId t;
  const TValue *slot;
  lhat_lock(L);
  api_checknelems(L, 1);
  t = index2addr(L, idx);
  if (lhatV_fastset(L, t, n, slot, lhatH_getint, L->top - 1))
    L->top--;  // pop value
  else {
    setivalue(L->top, n);
    api_incr_top(L);
    lhatV_finishset(L, t, L->top - 1, L->top - 2, slot);
    L->top -= 2;  // pop value and key
  }
  lhat_unlock(L);
}


LHAT_API void lhat_rawset (lhat_State *L, int idx) {
  StkId o;
  TValue *slot;
  lhat_lock(L);
  api_checknelems(L, 2);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  slot = lhatH_set(L, hvalue(o), L->top - 2);
  setobj2t(L, slot, L->top - 1);
  invalidateTMcache(hvalue(o));
  lhatC_barrierback(L, hvalue(o), L->top-1);
  L->top -= 2;
  lhat_unlock(L);
}


LHAT_API void lhat_rawseti (lhat_State *L, int idx, lhat_Integer n) {
  StkId o;
  lhat_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  lhatH_setint(L, hvalue(o), n, L->top - 1);
  lhatC_barrierback(L, hvalue(o), L->top-1);
  L->top--;
  lhat_unlock(L);
}


LHAT_API void lhat_rawsetp (lhat_State *L, int idx, const void *p) {
  StkId o;
  TValue k, *slot;
  lhat_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  setpvalue(&k, cast(void *, p));
  slot = lhatH_set(L, hvalue(o), &k);
  setobj2t(L, slot, L->top - 1);
  lhatC_barrierback(L, hvalue(o), L->top - 1);
  L->top--;
  lhat_unlock(L);
}


LHAT_API int lhat_setmetatable (lhat_State *L, int objindex) {
  TValue *obj;
  Table *mt;
  lhat_lock(L);
  api_checknelems(L, 1);
  obj = index2addr(L, objindex);
  if (ttisnil(L->top - 1))
    mt = NULL;
  else {
    api_check(L, ttistable(L->top - 1), "table expected");
    mt = hvalue(L->top - 1);
  }
  switch (ttnov(obj)) {
    case LHAT_TTABLE: {
      hvalue(obj)->metatable = mt;
      if (mt) {
        lhatC_objbarrier(L, gcvalue(obj), mt);
        lhatC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    case LHAT_TUSERDATA: {
      uvalue(obj)->metatable = mt;
      if (mt) {
        lhatC_objbarrier(L, uvalue(obj), mt);
        lhatC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    default: {
      G(L)->mt[ttnov(obj)] = mt;
      break;
    }
  }
  L->top--;
  lhat_unlock(L);
  return 1;
}


LHAT_API void lhat_setuservalue (lhat_State *L, int idx) {
  StkId o;
  lhat_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  setuservalue(L, uvalue(o), L->top - 1);
  lhatC_barrier(L, gcvalue(o), L->top - 1);
  L->top--;
  lhat_unlock(L);
}


//
// 'load' and 'call' functions (run Lhat code)
//


#define checkresults(L,na,nr) \
     api_check(L, (nr) == LHAT_MULTRET || (L->ci->top - L->top >= (nr) - (na)), \
	"results from function overflow current stack size")


LHAT_API void lhat_callk (lhat_State *L, int nargs, int nresults,
                        lhat_KContext ctx, lhat_KFunction k) {
  StkId func;
  lhat_lock(L);
  api_check(L, k == NULL || !isLhat(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LHAT_OK, "cannot do calls on non-normal coroutine");
  checkresults(L, nargs, nresults);
  func = L->top - (nargs+1);
  if (k != NULL && L->nny == 0) {  // need to prepare continuation?
    L->ci->u.c.k = k;  // save continuation
    L->ci->u.c.ctx = ctx;  // save context
    lhatD_call(L, func, nresults);  // do the call
  }
  else  // no continuation or no yieldable
    lhatD_callnoyield(L, func, nresults);  // just do the call
  adjustresults(L, nresults);
  lhat_unlock(L);
}



//
// Execute a protected call.
//
struct CallS {  // data to 'f_call'
  StkId func;
  int nresults;
};


static void f_call (lhat_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);
  lhatD_callnoyield(L, c->func, c->nresults);
}



LHAT_API int lhat_pcallk (lhat_State *L, int nargs, int nresults, int errfunc,
                        lhat_KContext ctx, lhat_KFunction k) {
  struct CallS c;
  int status;
  ptrdiff_t func;
  lhat_lock(L);
  api_check(L, k == NULL || !isLhat(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LHAT_OK, "cannot do calls on non-normal coroutine");
  checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else {
    StkId o = index2addr(L, errfunc);
    api_checkstackindex(L, errfunc, o);
    func = savestack(L, o);
  }
  c.func = L->top - (nargs+1);  // function to be called
  if (k == NULL || L->nny > 0) {  // no continuation or no yieldable?
    c.nresults = nresults;  // do a 'conventional' protected call
    status = lhatD_pcall(L, f_call, &c, savestack(L, c.func), func);
  }
  else {  // prepare continuation (call is already protected by 'resume')
    CallInfo *ci = L->ci;
    ci->u.c.k = k;  // save continuation
    ci->u.c.ctx = ctx;  // save context
    // save information for error recovery
    ci->extra = savestack(L, c.func);
    ci->u.c.old_errfunc = L->errfunc;
    L->errfunc = func;
    setoah(ci->callstatus, L->allowhook);  // save value of 'allowhook'
    ci->callstatus |= CIST_YPCALL;  // function can do error recovery
    lhatD_call(L, c.func, nresults);  // do the call
    ci->callstatus &= ~CIST_YPCALL;
    L->errfunc = ci->u.c.old_errfunc;
    status = LHAT_OK;  // if it is here, there were no errors
  }
  adjustresults(L, nresults);
  lhat_unlock(L);
  return status;
}


LHAT_API int lhat_load (lhat_State *L, lhat_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  ZIO z;
  int status;
  lhat_lock(L);
  if (!chunkname) chunkname = "?";
  lhatZ_init(L, &z, reader, data);
  status = lhatD_protectedparser(L, &z, chunkname, mode);
  if (status == LHAT_OK) {  // no errors?
    LClosure *f = clLvalue(L->top - 1);  // get newly created function
    if (f->nupvalues >= 1) {  // does it have an upvalues?
      // get global table from registry
      Table *reg = hvalue(&G(L)->l_registry);
      const TValue *gt = lhatH_getint(reg, LHAT_RIDX_GLOBALS);
      // set global table as 1st upvalues of 'f' (may be LHAT_ENV)
      setobj(L, f->upvalues[0]->v, gt);
      lhatC_upvalbarrier(L, f->upvalues[0]);
    }
  }
  lhat_unlock(L);
  return status;
}


LHAT_API int lhat_dump (lhat_State *L, lhat_Writer writer, void *data, int strip) {
  int status;
  TValue *o;
  lhat_lock(L);
  api_checknelems(L, 1);
  o = L->top - 1;
  if (isLfunction(o))
    status = lhatU_dump(L, getproto(o), writer, data, strip);
  else
    status = 1;
  lhat_unlock(L);
  return status;
}


LHAT_API int lhat_status (lhat_State *L) {
  return L->status;
}


//
// Garbage-collection function
//

LHAT_API int lhat_gc (lhat_State *L, int what, int data) {
  int res = 0;
  global_State *g;
  lhat_lock(L);
  g = G(L);
  switch (what) {
    case LHAT_GCSTOP: {
      g->gcrunning = 0;
      break;
    }
    case LHAT_GCRESTART: {
      lhatE_setdebt(g, 0);
      g->gcrunning = 1;
      break;
    }
    case LHAT_GCCOLLECT: {
      lhatC_fullgc(L, 0);
      break;
    }
    case LHAT_GCCOUNT: {
      // GC values are expressed in Kbytes: #bytes/2^10
      res = cast_int(gettotalbytes(g) >> 10);
      break;
    }
    case LHAT_GCCOUNTB: {
      res = cast_int(gettotalbytes(g) & 0x3ff);
      break;
    }
    case LHAT_GCSTEP: {
      l_mem debt = 1;  // =1 to signal that it did an actual step
      lu_byte oldrunning = g->gcrunning;
      g->gcrunning = 1;  // allow GC to run
      if (data == 0) {
        lhatE_setdebt(g, -GCSTEPSIZE);  // to do a "small" step
        lhatC_step(L);
      }
      else {  // add 'data' to total debt
        debt = cast(l_mem, data) * 1024 + g->GCdebt;
        lhatE_setdebt(g, debt);
        lhatC_checkGC(L);
      }
      g->gcrunning = oldrunning;  // restore previous state
      if (debt > 0 && g->gcstate == GCSpause)  // end of cycle?
        res = 1;  // signal it
      break;
    }
    case LHAT_GCSETPAUSE: {
      res = g->gcpause;
      g->gcpause = data;
      break;
    }
    case LHAT_GCSETSTEPMUL: {
      res = g->gcstepmul;
      if (data < 40) data = 40;  // avoid ridiculous low values (and 0)
      g->gcstepmul = data;
      break;
    }
    case LHAT_GCISRUNNING: {
      res = g->gcrunning;
      break;
    }
    default: res = -1;  // invalid option
  }
  lhat_unlock(L);
  return res;
}



//
// miscellaneous functions
//


LHAT_API int lhat_error (lhat_State *L) {
  lhat_lock(L);
  api_checknelems(L, 1);
  lhatG_errormsg(L);
  // code unreachable; will unlock when control actually leaves the kernel
  return 0;  // to avoid warnings
}


LHAT_API int lhat_next (lhat_State *L, int idx) {
  StkId t;
  int more;
  lhat_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  more = lhatH_next(L, hvalue(t), L->top - 1);
  if (more) {
    api_incr_top(L);
  }
  else  // no more elements
    L->top -= 1;  // remove key
  lhat_unlock(L);
  return more;
}


LHAT_API void lhat_concat (lhat_State *L, int n) {
  lhat_lock(L);
  api_checknelems(L, n);
  if (n >= 2) {
    lhatV_concat(L, n);
  }
  else if (n == 0) {  // push empty string
    setsvalue2s(L, L->top, lhatS_newlstr(L, "", 0));
    api_incr_top(L);
  }
  // else n == 1; nothing to do
  lhatC_checkGC(L);
  lhat_unlock(L);
}


LHAT_API void lhat_len (lhat_State *L, int idx) {
  StkId t;
  lhat_lock(L);
  t = index2addr(L, idx);
  lhatV_objlen(L, L->top, t);
  api_incr_top(L);
  lhat_unlock(L);
}


LHAT_API lhat_Alloc lhat_getallocf (lhat_State *L, void **ud) {
  lhat_Alloc f;
  lhat_lock(L);
  if (ud) *ud = G(L)->ud;
  f = G(L)->frealloc;
  lhat_unlock(L);
  return f;
}


LHAT_API void lhat_setallocf (lhat_State *L, lhat_Alloc f, void *ud) {
  lhat_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lhat_unlock(L);
}


LHAT_API void *lhat_newuserdata (lhat_State *L, size_t size) {
  Udata *u;
  lhat_lock(L);
  u = lhatS_newudata(L, size);
  setuvalue(L, L->top, u);
  api_incr_top(L);
  lhatC_checkGC(L);
  lhat_unlock(L);
  return getudatamem(u);
}



static const char *aux_upvalue (StkId fi, int n, TValue **val,
                                CClosure **owner, Upvalue **uv) {
  switch (ttype(fi)) {
    case LHAT_TCCL: {  // C closure
      CClosure *f = clCvalue(fi);
      if (!(1 <= n && n <= f->nupvalues)) return NULL;
      *val = &f->upvalues[n-1];
      if (owner) *owner = f;
      return "";
    }
    case LHAT_TLCL: {  // Lhat closure
      LClosure *f = clLvalue(fi);
      TString *name;
      Proto *p = f->p;
      if (!(1 <= n && n <= p->sizeupvalues)) return NULL;
      *val = f->upvalues[n-1]->v;
      if (uv) *uv = f->upvalues[n - 1];
      name = p->upvalues[n-1].name;
      return (name == NULL) ? "(*no name)" : getstr(name);
    }
    default: return NULL;  // not a closure
  }
}


LHAT_API const char *lhat_getupvalue (lhat_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  // to avoid warnings
  lhat_lock(L);
  name = aux_upvalue(index2addr(L, funcindex), n, &val, NULL, NULL);
  if (name) {
    setobj2s(L, L->top, val);
    api_incr_top(L);
  }
  lhat_unlock(L);
  return name;
}


LHAT_API const char *lhat_setupvalue (lhat_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  // to avoid warnings
  CClosure *owner = NULL;
  Upvalue *uv = NULL;
  StkId fi;
  lhat_lock(L);
  fi = index2addr(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(fi, n, &val, &owner, &uv);
  if (name) {
    L->top--;
    setobj(L, val, L->top);
    if (owner) { lhatC_barrier(L, owner, L->top); }
    else if (uv) { lhatC_upvalbarrier(L, uv); }
  }
  lhat_unlock(L);
  return name;
}


static Upvalue **getupvalref (lhat_State *L, int fidx, int n, LClosure **pf) {
  LClosure *f;
  StkId fi = index2addr(L, fidx);
  api_check(L, ttisLclosure(fi), "Lhat function expected");
  f = clLvalue(fi);
  api_check(L, (1 <= n && n <= f->p->sizeupvalues), "invalid upvalue index");
  if (pf) *pf = f;
  return &f->upvalues[n - 1];  // get its upvalues pointer
}


LHAT_API void *lhat_upvalueid (lhat_State *L, int fidx, int n) {
  StkId fi = index2addr(L, fidx);
  switch (ttype(fi)) {
    case LHAT_TLCL: {  // lhat closure
      return *getupvalref(L, fidx, n, NULL);
    }
    case LHAT_TCCL: {  // C closure
      CClosure *f = clCvalue(fi);
      api_check(L, 1 <= n && n <= f->nupvalues, "invalid upvalue index");
      return &f->upvalues[n - 1];
    }
    default: {
      api_check(L, 0, "closure expected");
      return NULL;
    }
  }
}


LHAT_API void lhat_upvaluejoin (lhat_State *L, int fidx1, int n1,
                                            int fidx2, int n2) {
  LClosure *f1;
  Upvalue **up1 = getupvalref(L, fidx1, n1, &f1);
  Upvalue **up2 = getupvalref(L, fidx2, n2, NULL);
  lhatC_upvdeccount(L, *up1);
  *up1 = *up2;
  (*up1)->refcount++;
  if (upisopen(*up1)) (*up1)->u.open.touched = 1;
  lhatC_upvalbarrier(L, *up1);
}


