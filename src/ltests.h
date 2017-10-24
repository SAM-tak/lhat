#ifndef ltests_h
#define ltests_h
//
// Internal Header for Debugging of the L^ Implementation
// See Copyright Notice in lhat.h
//

#include <stdlib.h>


#define LHAT_DEBUG


// turn on assertions
#undef NDEBUG
#include <assert.h>
#define lhat_assert(c)           assert(c)


// to avoid warnings, and to make sure value is really unused
#define UNUSED(x)       (x=0, (void)(x))


// test for sizes in 'l_sprintf' (make sure whole buffer is available)
#undef l_sprintf
#if !defined(LHAT_USE_C89)
#define l_sprintf(s,sz,f,i)	(memset(s,0xAB,sz), snprintf(s,sz,f,i))
#else
#define l_sprintf(s,sz,f,i)	(memset(s,0xAB,sz), sprintf(s,f,i))
#endif


// memory-allocator control variables
typedef struct Memcontrol {
	unsigned long numblocks;
	unsigned long total;
	unsigned long maxmem;
	unsigned long memlimit;
	unsigned long objcount[LHAT_NUMTAGS];
} Memcontrol;

LHAT_API Memcontrol l_memcontrol;


//
// generic variable for debug tricks
//
extern void *l_Trick;



//
// Function to traverse and check all memory used by L^
//
int lhat_checkmemory(lhat_State *L);


// test for lock/unlock

struct L_EXTRA { int lock; int *plock; };
#undef LHAT_EXTRASPACE
#define LHAT_EXTRASPACE	sizeof(struct L_EXTRA)
#define getlock(l)	cast(struct L_EXTRA*, lhat_getextraspace(l))
#define lhati_userstateopen(l)  \
	(getlock(l)->lock = 0, getlock(l)->plock = &(getlock(l)->lock))
#define lhati_userstateclose(l)  \
  lhat_assert(getlock(l)->lock == 1 && getlock(l)->plock == &(getlock(l)->lock))
#define lhati_userstatecoroutine(l,l1) \
  lhat_assert(getlock(l1)->plock == getlock(l)->plock)
#define lhati_userstatefree(l,l1) \
  lhat_assert(getlock(l)->plock == getlock(l1)->plock)
#define lhat_lock(l)     lhat_assert((*getlock(l)->plock)++ == 0)
#define lhat_unlock(l)   lhat_assert(--(*getlock(l)->plock) == 0)



LHAT_API int lhatB_opentests(lhat_State *L);

LHAT_API void *debug_realloc(void *ud, void *block, size_t osize, size_t nsize);

// change some sizes to give some bugs a chance

#undef LHATL_BUFFERSIZE
#define LHATL_BUFFERSIZE		23
#define MINSTRTABSIZE		2
#define MAXINDEXRK		1


// make stack-overflow tests run faster
#undef LHATI_MAXSTACK
#define LHATI_MAXSTACK   50000


#undef LHATI_USER_ALIGNMENT_T
#define LHATI_USER_ALIGNMENT_T   union { char b[sizeof(void*) * 8]; }


#define STRCACHE_N	23
#define STRCACHE_M	5

#endif