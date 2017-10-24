//
// $Id: lbitlib.c,v 1.30 2015/11/11 19:08:09 roberto Exp $
// Standard library for bitwise operations
// See Copyright Notice in lhat.h
//

#define lbitlib_c
#define LHAT_LIB

#include "lprefix.h"


#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"


#if defined(LHAT_COMPAT_BITLIB)		// {


#define pushunsigned(L,n)	lhat_pushinteger(L, (lhat_Integer)(n))
#define checkunsigned(L,i)	((lhat_Unsigned)lhatL_checkinteger(L,i))


// number of bits to consider in a number
#if !defined(LHAT_NBITS)
#define LHAT_NBITS	32
#endif


//
// a lhat_Unsigned with its first LHAT_NBITS bits equal to 1. (Shift must
// be made in two parts to avoid problems when LHAT_NBITS is equal to the
// number of bits in a lhat_Unsigned.)
//
#define ALLONES		(~(((~(lhat_Unsigned)0) << (LHAT_NBITS - 1)) << 1))


// macro to trim extra bits
#define trim(x)		((x) & ALLONES)


// builds a number with 'n' ones (1 <= n <= LHAT_NBITS)
#define mask(n)		(~((ALLONES << 1) << ((n) - 1)))



static lhat_Unsigned andaux (lhat_State *L) {
  int i, n = lhat_gettop(L);
  lhat_Unsigned r = ~(lhat_Unsigned)0;
  for (i = 1; i <= n; i++)
    r &= checkunsigned(L, i);
  return trim(r);
}


static int b_and (lhat_State *L) {
  lhat_Unsigned r = andaux(L);
  pushunsigned(L, r);
  return 1;
}


static int b_test (lhat_State *L) {
  lhat_Unsigned r = andaux(L);
  lhat_pushboolean(L, r != 0);
  return 1;
}


static int b_or (lhat_State *L) {
  int i, n = lhat_gettop(L);
  lhat_Unsigned r = 0;
  for (i = 1; i <= n; i++)
    r |= checkunsigned(L, i);
  pushunsigned(L, trim(r));
  return 1;
}


static int b_xor (lhat_State *L) {
  int i, n = lhat_gettop(L);
  lhat_Unsigned r = 0;
  for (i = 1; i <= n; i++)
    r ^= checkunsigned(L, i);
  pushunsigned(L, trim(r));
  return 1;
}


static int b_not (lhat_State *L) {
  lhat_Unsigned r = ~checkunsigned(L, 1);
  pushunsigned(L, trim(r));
  return 1;
}


static int b_shift (lhat_State *L, lhat_Unsigned r, lhat_Integer i) {
  if (i < 0) {  // shift right?
    i = -i;
    r = trim(r);
    if (i >= LHAT_NBITS) r = 0;
    else r >>= i;
  }
  else {  // shift left
    if (i >= LHAT_NBITS) r = 0;
    else r <<= i;
    r = trim(r);
  }
  pushunsigned(L, r);
  return 1;
}


static int b_lshift (lhat_State *L) {
  return b_shift(L, checkunsigned(L, 1), lhatL_checkinteger(L, 2));
}


static int b_rshift (lhat_State *L) {
  return b_shift(L, checkunsigned(L, 1), -lhatL_checkinteger(L, 2));
}


static int b_arshift (lhat_State *L) {
  lhat_Unsigned r = checkunsigned(L, 1);
  lhat_Integer i = lhatL_checkinteger(L, 2);
  if (i < 0 || !(r & ((lhat_Unsigned)1 << (LHAT_NBITS - 1))))
    return b_shift(L, r, -i);
  else {  // arithmetic shift for 'negative' number
    if (i >= LHAT_NBITS) r = ALLONES;
    else
      r = trim((r >> i) | ~(trim(~(lhat_Unsigned)0) >> i));  // add signal bit
    pushunsigned(L, r);
    return 1;
  }
}


static int b_rot (lhat_State *L, lhat_Integer d) {
  lhat_Unsigned r = checkunsigned(L, 1);
  int i = d & (LHAT_NBITS - 1);  // i = d % NBITS
  r = trim(r);
  if (i != 0)  // avoid undefined shift of LHAT_NBITS when i == 0
    r = (r << i) | (r >> (LHAT_NBITS - i));
  pushunsigned(L, trim(r));
  return 1;
}


static int b_lrot (lhat_State *L) {
  return b_rot(L, lhatL_checkinteger(L, 2));
}


static int b_rrot (lhat_State *L) {
  return b_rot(L, -lhatL_checkinteger(L, 2));
}


//
// get field and width arguments for field-manipulation functions,
// checking whether they are valid.
// ('lhatL_error' called without 'return' to avoid later warnings about
// 'width' being used uninitialized.)
//
static int fieldargs (lhat_State *L, int farg, int *width) {
  lhat_Integer f = lhatL_checkinteger(L, farg);
  lhat_Integer w = lhatL_optinteger(L, farg + 1, 1);
  lhatL_argcheck(L, 0 <= f, farg, "field cannot be negative");
  lhatL_argcheck(L, 0 < w, farg + 1, "width must be positive");
  if (f + w > LHAT_NBITS)
    lhatL_error(L, "trying to access non-existent bits");
  *width = (int)w;
  return (int)f;
}


static int b_extract (lhat_State *L) {
  int w;
  lhat_Unsigned r = trim(checkunsigned(L, 1));
  int f = fieldargs(L, 2, &w);
  r = (r >> f) & mask(w);
  pushunsigned(L, r);
  return 1;
}


static int b_replace (lhat_State *L) {
  int w;
  lhat_Unsigned r = trim(checkunsigned(L, 1));
  lhat_Unsigned v = trim(checkunsigned(L, 2));
  int f = fieldargs(L, 3, &w);
  lhat_Unsigned m = mask(w);
  r = (r & ~(m << f)) | ((v & m) << f);
  pushunsigned(L, r);
  return 1;
}


static const lhatL_Reg bitlib[] = {
  {"arshift", b_arshift},
  {"band", b_and},
  {"bnot", b_not},
  {"bor", b_or},
  {"bxor", b_xor},
  {"btest", b_test},
  {"extract", b_extract},
  {"lrotate", b_lrot},
  {"lshift", b_lshift},
  {"replace", b_replace},
  {"rrotate", b_rrot},
  {"rshift", b_rshift},
  {NULL, NULL}
};



LHATMOD_API int lhatopen_bit32 (lhat_State *L) {
  lhatL_newlib(L, bitlib);
  return 1;
}


#else					// }{


LHATMOD_API int lhatopen_bit32 (lhat_State *L) {
  return lhatL_error(L, "library 'bit32' has been deprecated");
}

#endif					// }
