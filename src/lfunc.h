/*
** $Id: lfunc.h,v 2.15 2015/01/13 15:49:11 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lhat.h
*/

#ifndef lfunc_h
#define lfunc_h


#include "lobject.h"


#define sizeCclosure(n)	(cast(int, sizeof(CClosure)) + \
                         cast(int, sizeof(TValue)*((n)-1)))

#define sizeLclosure(n)	(cast(int, sizeof(LClosure)) + \
                         cast(int, sizeof(TValue *)*((n)-1)))


/* test whether coroutine is in 'twups' list */
#define isintwups(L)	(L->twups != L)


/*
** maximum number of upvalues in a closure (both C and Lhat). (Value
** must fit in a VM register.)
*/
#define MAXUPVAL	255


/*
** Upvalues for Lhat closures
*/
struct UpVal {
  TValue *v;  /* points to stack or to its own value */
  lu_mem refcount;  /* reference counter */
  union {
    struct {  /* (when open) */
      UpVal *next;  /* linked list */
      int touched;  /* mark to avoid cycles with dead coroutines */
    } open;
    TValue value;  /* the value (when closed) */
  } u;
};

#define upisopen(up)	((up)->v != &(up)->u.value)


LHATI_FUNC Proto *lhatF_newproto (lhat_State *L);
LHATI_FUNC CClosure *lhatF_newCclosure (lhat_State *L, int nelems);
LHATI_FUNC LClosure *lhatF_newLclosure (lhat_State *L, int nelems);
LHATI_FUNC void lhatF_initupvals (lhat_State *L, LClosure *cl);
LHATI_FUNC UpVal *lhatF_findupval (lhat_State *L, StkId level);
LHATI_FUNC void lhatF_close (lhat_State *L, StkId level);
LHATI_FUNC void lhatF_freeproto (lhat_State *L, Proto *f);
LHATI_FUNC const char *lhatF_getlocalname (const Proto *func, int local_number,
                                         int pc);


#endif
