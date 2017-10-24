#ifndef lvm_h
#define lvm_h
//
// L^ virtual machine
// See Copyright Notice in lhat.h
//

#include "ldo.h"
#include "lobject.h"
#include "lmetamethods.h"

#if !defined(LHAT_NOCVTN2S)
# define cvt2str(o)	ttisnumber(o)
#else
# define cvt2str(o)	0	// no conversion from numbers to strings
#endif


#if !defined(LHAT_NOCVTS2N)
# define cvt2num(o)	ttisstring(o)
#else
# define cvt2num(o)	0	// no conversion from strings to numbers
#endif


//
// You can define LHAT_FLOORN2I if you want to convert floats to integers
// by flooring them (instead of raising an error if they are not
// integral values)
//
#if !defined(LHAT_FLOORN2I)
#define LHAT_FLOORN2I		0
#endif


#define tonumber(o,n) \
	(ttisfloat(o) ? (*(n) = fltvalue(o), 1) : lhatV_tonumber_(o,n))

#define tointeger(o,i) \
    (ttisinteger(o) ? (*(i) = ivalue(o), 1) : lhatV_tointeger(o,i,LHAT_FLOORN2I))

#define intop(op,v1,v2) l_castU2S(l_castS2U(v1) op l_castS2U(v2))

#define lhatV_rawequalobj(t1,t2)		lhatV_equalobj(NULL,t1,t2)


//
// fast track for 'gettable': if 't' is a table and 't[k]' is not nil,
// return 1 with 'slot' pointing to 't[k]' (final result).  Otherwise,
// return 0 (meaning it will have to check metamethod) with 'slot'
// pointing to a nil 't[k]' (if 't' is a table) or NULL (otherwise).
// 'f' is the raw get function to use.
//
#define lhatV_fastget(L,t,k,slot,f) \
  (!ttistable(t)  \
   ? (slot = NULL, 0)  /* not a table; 'slot' is NULL and result is 0 */\
   : (slot = f(hvalue(t), k),  /* else, do raw access */\
      !ttisnil(slot)))  /* result not nil? */

//
// standard implementation for 'gettable'
//
#define lhatV_gettable(L,t,k,v) { const TValue *slot; \
  if (lhatV_fastget(L,t,k,slot,lhatH_get)) { setobj2s(L, v, slot); } \
  else lhatV_finishget(L,t,k,v,slot); }


//
// Fast track for set table. If 't' is a table and 't[k]' is not nil,
// call GC barrier, do a raw 't[k]=v', and return true; otherwise,
// return false with 'slot' equal to NULL (if 't' is not a table) or
// 'nil'. (This is needed by 'lhatV_finishget'.) Note that, if the macro
// returns true, there is no need to 'invalidateTMcache', because the
// call is not creating a new entry.
//
#define lhatV_fastset(L,t,k,slot,f,v) \
  (!ttistable(t) \
   ? (slot = NULL, 0) \
   : (slot = f(hvalue(t), k), \
     ttisnil(slot) ? 0 \
     : (lhatC_barrierback(L, hvalue(t), v), \
        setobj2t(L, cast(TValue *,slot), v), \
        1)))


#define lhatV_settable(L,t,k,v) { const TValue *slot; \
  if (!lhatV_fastset(L,t,k,slot,lhatH_get,v)) \
    lhatV_finishset(L,t,k,v,slot); }



LHATI_FUNC int lhatV_equalobj(lhat_State *L, const TValue *t1, const TValue *t2);
LHATI_FUNC int lhatV_lessthan(lhat_State *L, const TValue *l, const TValue *r);
LHATI_FUNC int lhatV_lessequal(lhat_State *L, const TValue *l, const TValue *r);
LHATI_FUNC int lhatV_tonumber_(const TValue *obj, lhat_Number *n);
LHATI_FUNC int lhatV_tointeger(const TValue *obj, lhat_Integer *p, int mode);
LHATI_FUNC void lhatV_finishget(lhat_State *L, const TValue *t, TValue *key, StkId val, const TValue *slot);
LHATI_FUNC void lhatV_finishset(lhat_State *L, const TValue *t, TValue *key, StkId val, const TValue *slot);
LHATI_FUNC void lhatV_finishOp(lhat_State *L);
LHATI_FUNC void lhatV_execute(lhat_State *L);
LHATI_FUNC void lhatV_concat(lhat_State *L, int total);
LHATI_FUNC lhat_Integer lhatV_div(lhat_State *L, lhat_Integer x, lhat_Integer y);
LHATI_FUNC lhat_Integer lhatV_mod(lhat_State *L, lhat_Integer x, lhat_Integer y);
LHATI_FUNC lhat_Integer lhatV_shiftl(lhat_Integer x, lhat_Integer y);
LHATI_FUNC void lhatV_objlen(lhat_State *L, StkId ra, const TValue *rb);

#endif
