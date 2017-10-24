//
// $Id: llimits.h,v 1.141 2015/11/19 19:16:22 roberto Exp $
// Limits, basic types, and some other 'installation-dependent' definitions
// See Copyright Notice in lhat.h
//

#ifndef llimits_h
#define llimits_h


#include <limits.h>
#include <stddef.h>


#include "lhat.h"

//
// 'lu_mem' and 'l_mem' are unsigned/signed integers big enough to count
// the total memory used by Lhat (in bytes). Usually, 'size_t' and
// 'ptrdiff_t' should work, but we use 'long' for 16-bit machines.
//
#if defined(LHATI_MEM)		// { external definitions?
typedef LHATI_UMEM lu_mem;
typedef LHATI_MEM l_mem;
#elif LHATI_BITSINT >= 32	// }{
typedef size_t lu_mem;
typedef ptrdiff_t l_mem;
#else  // 16-bit ints	// }{
typedef unsigned long lu_mem;
typedef long l_mem;
#endif				// }


// chars used as small naturals (so that 'char' is reserved for characters)
typedef unsigned char lu_byte;


// maximum value for size_t
#define MAX_SIZET	((size_t)(~(size_t)0))

// maximum size visible for Lhat (must be representable in a lhat_Integer
#define MAX_SIZE	(sizeof(size_t) < sizeof(lhat_Integer) ? MAX_SIZET \
                          : (size_t)(LHAT_MAXINTEGER))


#define MAX_LUMEM	((lu_mem)(~(lu_mem)0))

#define MAX_LMEM	((l_mem)(MAX_LUMEM >> 1))


#define MAX_INT		INT_MAX  // maximum value of an int


//
// conversion of pointer to unsigned integer:
// this is for hashing only; there is no problem if the integer
// cannot hold the whole pointer value
//
#define point2uint(p)	((unsigned int)((size_t)(p) & UINT_MAX))



// type to ensure maximum alignment
#if defined(LHATI_USER_ALIGNMENT_T)
typedef LHATI_USER_ALIGNMENT_T L_Umaxalign;
#else
typedef union {
  lhat_Number n;
  double u;
  void *s;
  lhat_Integer i;
  long l;
} L_Umaxalign;
#endif



// types of 'usual argument conversions' for lhat_Number and lhat_Integer
typedef LHATI_UACNUMBER l_uacNumber;
typedef LHATI_UACINT l_uacInt;


// internal assertions for in-house debugging
#if defined(lhat_assert)
#define check_exp(c,e)		(lhat_assert(c), (e))
// to avoid problems with conditions too long
#define lhat_longassert(c)	((c) ? (void)0 : lhat_assert(0))
#else
#define lhat_assert(c)		((void)0)
#define check_exp(c,e)		(e)
#define lhat_longassert(c)	((void)0)
#endif

//
// assertion for checking API calls
//
#if !defined(lhati_apicheck)
#define lhati_apicheck(l,e)	lhat_assert(e)
#endif

#define api_check(l,e,msg)	lhati_apicheck(l,(e) && msg)


// macro to avoid warnings about unused variables
#if !defined(UNUSED)
#define UNUSED(x)	((void)(x))
#endif


// type casts (a macro highlights casts in the code)
#define cast(t, exp)	((t)(exp))

#define cast_void(i)	cast(void, (i))
#define cast_byte(i)	cast(lu_byte, (i))
#define cast_num(i)	cast(lhat_Number, (i))
#define cast_int(i)	cast(int, (i))
#define cast_uchar(i)	cast(unsigned char, (i))


// cast a signed lhat_Integer to lhat_Unsigned
#if !defined(l_castS2U)
#define l_castS2U(i)	((lhat_Unsigned)(i))
#endif

//
// cast a lhat_Unsigned to a signed lhat_Integer; this cast is
// not strict ISO C, but two-complement architectures should
// work fine.
//
#if !defined(l_castU2S)
#define l_castU2S(i)	((lhat_Integer)(i))
#endif


//
// non-return type
//
#if defined(__GNUC__)
#define l_noret		void __attribute__((noreturn))
#elif defined(_MSC_VER) && _MSC_VER >= 1200
#define l_noret		void __declspec(noreturn)
#else
#define l_noret		void
#endif



//
// maximum depth for nested C calls and syntactical nested non-terminals
// in a program. (Value must fit in an unsigned short int.)
//
#if !defined(LHATI_MAXCCALLS)
#define LHATI_MAXCCALLS		200
#endif



//
// type for virtual-machine instructions;
// must be an unsigned with (at least) 4 bytes (see details in lopcodes.h)
//
#if LHATI_BITSINT >= 32
typedef unsigned int Instruction;
#else
typedef unsigned long Instruction;
#endif



//
// Maximum length for short strings, that is, strings that are
// internalized. (Cannot be smaller than reserved words or tags for
// metamethods, as these strings must be internalized;
// #("function") = 8, #("__newindex") = 10.)
//
#if !defined(LHATI_MAXSHORTLEN)
#define LHATI_MAXSHORTLEN	40
#endif


//
// Initial size for the string table (must be power of 2).
// The Lhat core alone registers ~50 strings (reserved words +
// metaevent keys + a few others). Libraries would typically add
// a few dozens more.
//
#if !defined(MINSTRTABSIZE)
#define MINSTRTABSIZE	128
#endif


//
// Size of cache for strings in the API. 'N' is the number of
// sets (better be a prime) and "M" is the size of each set (M == 1
// makes a direct cache.)
//
#if !defined(STRCACHE_N)
#define STRCACHE_N		53
#define STRCACHE_M		2
#endif


// minimum size for string buffer
#if !defined(LHAT_MINBUFFER)
#define LHAT_MINBUFFER	32
#endif


//
// macros that are executed whenever program enters the Lhat core
// ('lhat_lock') and leaves the core ('lhat_unlock')
//
#if !defined(lhat_lock)
#define lhat_lock(L)	((void) 0)
#define lhat_unlock(L)	((void) 0)
#endif

//
// macro executed during Lhat functions at points where the
// function can yield.
//
#if !defined(lhati_coroutineyield)
#define lhati_coroutineyield(L)	{lhat_unlock(L); lhat_lock(L);}
#endif


//
// these macros allow user-specific actions on coroutines when you defined
// LHATI_EXTRASPACE and need to do something extra when a coroutine is
// created/deleted/resumed/yielded.
//
#if !defined(lhati_userstateopen)
#define lhati_userstateopen(L)		((void)L)
#endif

#if !defined(lhati_userstateclose)
#define lhati_userstateclose(L)		((void)L)
#endif

#if !defined(lhati_userstatecoroutine)
#define lhati_userstatecoroutine(L,L1)	((void)L)
#endif

#if !defined(lhati_userstatefree)
#define lhati_userstatefree(L,L1)	((void)L)
#endif

#if !defined(lhati_userstateresume)
#define lhati_userstateresume(L,n)	((void)L)
#endif

#if !defined(lhati_userstateyield)
#define lhati_userstateyield(L,n)	((void)L)
#endif



//
// The lhati_num* macros define the primitive operations over numbers.
//

// floor division (defined as 'floor(a/b)')
#if !defined(lhati_numidiv)
#define lhati_numidiv(L,a,b)     ((void)L, l_floor(lhati_numdiv(L,a,b)))
#endif

// float division
#if !defined(lhati_numdiv)
#define lhati_numdiv(L,a,b)      ((a)/(b))
#endif

//
// modulo: defined as 'a - floor(a/b)*b'; this definition gives NaN when
// 'b' is huge, but the result should be 'a'. 'fmod' gives the result of
// 'a - trunc(a/b)*b', and therefore must be corrected when 'trunc(a/b)
// ~= floor(a/b)'. That happens when the division has a non-integer
// negative result, which is equivalent to the test below.
//
#if !defined(lhati_nummod)
#define lhati_nummod(L,a,b,m)  \
  { (m) = l_mathop(fmod)(a,b); if ((m)*(b) < 0) (m) += (b); }
#endif

// exponentiation
#if !defined(lhati_numpow)
#define lhati_numpow(L,a,b)      ((void)L, l_mathop(pow)(a,b))
#endif

// the others are quite standard operations
#if !defined(lhati_numadd)
#define lhati_numadd(L,a,b)      ((a)+(b))
#define lhati_numsub(L,a,b)      ((a)-(b))
#define lhati_nummul(L,a,b)      ((a)*(b))
#define lhati_numunm(L,a)        (-(a))
#define lhati_numeq(a,b)         ((a)==(b))
#define lhati_numlt(a,b)         ((a)<(b))
#define lhati_numle(a,b)         ((a)<=(b))
#define lhati_numisnan(a)        (!lhati_numeq((a), (a)))
#endif





//
// macro to control inclusion of some hard tests on stack reallocation
//
#if !defined(HARDSTACKTESTS)
#define condmovestack(L,pre,pos)	((void)0)
#else
// realloc stack keeping its size
#define condmovestack(L,pre,pos)  \
	{ int sz_ = (L)->stacksize; pre; lhatD_reallocstack((L), sz_); pos; }
#endif

#if !defined(HARDMEMTESTS)
#define condchangemem(L,pre,pos)	((void)0)
#else
#define condchangemem(L,pre,pos)  \
	{ if (G(L)->gcrunning) { pre; lhatC_fullgc(L, 0); pos; } }
#endif

#endif
