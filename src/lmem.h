#ifndef lmem_h
#define lmem_h
//
// Interface to Memory Manager
// See Copyright Notice in lhat.h
//

#include <stddef.h>

#include "llimits.h"
#include "lhat.h"


// not to be called directly
LHATI_FUNC void *lhatM_realloc_(lhat_State *L, void *block, size_t oldsize, size_t size);
LHATI_FUNC void *lhatM_growaux_(lhat_State *L, void *block, int *size, int size_elem, int limit, const char *what);

LHATI_FUNC l_noret lhatM_toobig(lhat_State *L);

//
// This macro reallocs a vector 'b' from 'on' to 'n' elements, where
// each element has size 'e'. In case of arithmetic overflow of the
// product 'n'*'e', it raises an error (calling 'lhatM_toobig'). Because
// 'e' is always constant, it avoids the runtime division MAX_SIZET/(e).
//
// (The macro is somewhat complex to avoid warnings:  The 'sizeof'
// comparison avoids a runtime comparison when overflow cannot occur.
// The compiler should be able to optimize the real test by itself, but
// when it does it, it may give a warning about "comparison is always
// false due to limited range of data type"; the +1 tricks the compiler,
// avoiding this warning but also this optimization.)
//

inline void *lhatM_reallocv(lhat_State *L, void *b, size_t on, size_t n, int e)
{
	if(sizeof(n) >= sizeof(size_t) && cast(size_t, n) + 1 > MAX_SIZET / e) lhatM_toobig(L);
	return lhatM_realloc_(L, b, on*e, n*e);
}

//
// Arrays of chars do not need any test
//
inline char *lhatM_reallocvchar(lhat_State *L, void *b, size_t on, size_t n)
{
	return cast(char *, lhatM_realloc_(L, b, on * sizeof(char), n * sizeof(char)));
}

inline void lhatM_freemem(lhat_State *L, void *b, size_t s)
{
	lhatM_realloc_(L, b, s, 0);
}

#define lhatM_free(L, b)		lhatM_realloc_(L, (b), sizeof(*(b)), 0)
#define lhatM_freearray(L, b, n)   lhatM_realloc_(L, (b), (n)*sizeof(*(b)), 0)

inline void *lhatM_malloc(lhat_State *L, size_t s)
{
	return lhatM_realloc_(L, NULL, 0, s);
}

#define lhatM_new(L,t)		cast(t *, lhatM_malloc(L, sizeof(t)))
#define lhatM_newvector(L,n,t) \
		cast(t *, lhatM_reallocv(L, NULL, 0, n, sizeof(t)))

inline void *lhatM_newobject(lhat_State *L, size_t tag, size_t s)
{
	return lhatM_realloc_(L, NULL, tag, (s));
}

#define lhatM_growvector(L,v,nelems,size,t,limit,e) \
          if ((nelems)+1 > (size)) \
            ((v)=cast(t *, lhatM_growaux_(L,v,&(size),sizeof(t),limit,e)))

#define lhatM_reallocvector(L, v,oldn,n,t) \
   ((v)=cast(t *, lhatM_reallocv(L, v, oldn, n, sizeof(t))))


#endif
