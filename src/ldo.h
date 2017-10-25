#ifndef lhat_ldo_h
#define lhat_ldo_h
//
// Stack and Call structure of L^
// See Copyright Notice in lhat.h
//

#include "lobject.h"
#include "lstate.h"
#include "lzbuf.h"

//
// Macro to check stack size and grow stack if needed.  Parameters
// 'pre'/'pos' allow the macro to preserve a pointer into the
// stack across reallocations, doing the work only when needed.
// 'condmovestack' is used in heavy tests to force a stack reallocation
// at every check.
//
#define lhatD_checkstackaux(L,n,pre,pos)  \
	if (L->stack_last - L->top <= (n)) \
	  { pre; lhatD_growstack(L, n); pos; } else { condmovestack(L,pre,pos); }

// In general, 'pre'/'pos' are empty (nothing to save)
#define lhatD_checkstack(L,n)	lhatD_checkstackaux(L,n,(void)0,(void)0)

inline TValue *restorestack(lhat_State *L, int n)
{
	return (TValue *)((char *)L->stack + n);
}

inline ptrdiff_t savestack(lhat_State *L, StkId p)
{
	return ((char *)p-(char *)L->stack);
}

// type of protected functions, to be ran by 'runprotected'
typedef void(*Pfunc)(lhat_State *L, void *ud);

LHATI_FUNC int lhatD_protectedparser(lhat_State *L, ZBuf *z, const char *name, const char *mode);
LHATI_FUNC void lhatD_hook(lhat_State *L, int event, int line);
LHATI_FUNC int lhatD_precall(lhat_State *L, StkId func, int nresults);
LHATI_FUNC void lhatD_call(lhat_State *L, StkId func, int nResults);
LHATI_FUNC void lhatD_callnoyield(lhat_State *L, StkId func, int nResults);
LHATI_FUNC int lhatD_pcall(lhat_State *L, Pfunc func, void *u, ptrdiff_t oldtop, ptrdiff_t ef);
LHATI_FUNC int lhatD_poscall(lhat_State *L, CallInfo *ci, StkId firstResult, int nres);
LHATI_FUNC void lhatD_reallocstack(lhat_State *L, int newsize);
LHATI_FUNC void lhatD_growstack(lhat_State *L, int n);
LHATI_FUNC void lhatD_shrinkstack(lhat_State *L);
LHATI_FUNC void lhatD_inctop(lhat_State *L);

LHATI_FUNC l_noret lhatD_throw(lhat_State *L, int errcode);
LHATI_FUNC int lhatD_rawrunprotected(lhat_State *L, Pfunc f, void *ud);

#endif // !lhat_ldo_h
