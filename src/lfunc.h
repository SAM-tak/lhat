#ifndef lhat_lfunc_h
#define lhat_lfunc_h
//
// Auxiliary functions to manipulate prototypes and closures
// See Copyright Notice in lhat.h
//

#include "lobject.h"

inline size_t sizeCclosure(int n)
{
	return cast(int, sizeof(CClosure)) + cast(int, sizeof(TValue)*n);
}

inline size_t sizeLclosure(int n)
{
	return cast(int, sizeof(LClosure)) + cast(int, sizeof(TValue *)*n);
}


// test whether coroutine is in 'cowups' list
#define isincowups(L)	(L->cowups != L)


//
// maximum number of upvalues in a closure (both C and L^). (Value
// must fit in a VM register.)
//
#define MAXUPVAL	255


//
// Upvalues for L^ closures
//
struct Upvalue {
	TValue *v;  // points to stack or to its own value
	lu_mem refcount;  // reference counter
	union {
		struct {  // (when open)
			Upvalue *next;  // linked list
			int touched;  // mark to avoid cycles with dead coroutines
		} open;
		TValue value;  // the value (when closed)
	} u;
};

#define upisopen(up)	((up)->v != &(up)->u.value)

LHATI_FUNC Proto *lhatF_newproto(lhat_State *L);
LHATI_FUNC CClosure *lhatF_newCclosure(lhat_State *L, int nelems);
LHATI_FUNC LClosure *lhatF_newLclosure(lhat_State *L, int nelems);
LHATI_FUNC void lhatF_initupvals(lhat_State *L, LClosure *cl);
LHATI_FUNC Upvalue *lhatF_findupval(lhat_State *L, StkId level);
LHATI_FUNC void lhatF_close(lhat_State *L, StkId level);
LHATI_FUNC void lhatF_freeproto(lhat_State *L, Proto *f);
LHATI_FUNC const char *lhatF_getlocalname(const Proto *func, int local_number, int pc);

#endif