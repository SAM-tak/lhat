#ifndef lapi_h
#define lapi_h
//
// Auxiliary functions from L^ API
// See Copyright Notice in lhat.h
//

#include "llimits.h"
#include "lstate.h"

inline void api_incr_top(lhat_State *L)
{
	L->top++;
	api_check(L, L->top <= L->ci->top, "stack overflow");
}

inline void adjustresults(lhat_State *L, int nres)
{
	if(nres == LHAT_MULTRET && L->ci->top < L->top) L->ci->top = L->top;
}

inline void api_checknelems(lhat_State *L, int n)
{
	api_check(L, n < (L->top - L->ci->func), "not enough elements in the stack");
}

#endif