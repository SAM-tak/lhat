//
// $Id: lmathlib.c,v 1.119 2016/12/22 13:08:50 roberto Exp $
// Standard mathematical library
// See Copyright Notice in lhat.h
//

#define lmathlib_c
#define LHAT_LIB

#include "lprefix.h"


#include <stdlib.h>
#include <math.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"


#undef PI
#define PI	(l_mathop(3.141592653589793238462643383279502884))


#if !defined(l_rand)		// {
#if defined(LHAT_USE_POSIX)
#define l_rand()	random()
#define l_srand(x)	srandom(x)
#define L_RANDMAX	2147483647	// (2^31 - 1), following POSIX
#else
#define l_rand()	rand()
#define l_srand(x)	srand(x)
#define L_RANDMAX	RAND_MAX
#endif
#endif				// }


static int math_abs (lhat_State *L) {
  if (lhat_isinteger(L, 1)) {
    lhat_Integer n = lhat_tointeger(L, 1);
    if (n < 0) n = (lhat_Integer)(0u - (lhat_Unsigned)n);
    lhat_pushinteger(L, n);
  }
  else
    lhat_pushnumber(L, l_mathop(fabs)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_sin (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(sin)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_cos (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(cos)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_tan (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(tan)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_asin (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(asin)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_acos (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(acos)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_atan (lhat_State *L) {
  lhat_Number y = lhatL_checknumber(L, 1);
  lhat_Number x = lhatL_optnumber(L, 2, 1);
  lhat_pushnumber(L, l_mathop(atan2)(y, x));
  return 1;
}


static int math_toint (lhat_State *L) {
  int valid;
  lhat_Integer n = lhat_tointegerx(L, 1, &valid);
  if (valid)
    lhat_pushinteger(L, n);
  else {
    lhatL_checkany(L, 1);
    lhat_pushnil(L);  // value is not convertible to integer
  }
  return 1;
}


static void pushnumint (lhat_State *L, lhat_Number d) {
  lhat_Integer n;
  if (lhat_numbertointeger(d, &n))  // does 'd' fit in an integer?
    lhat_pushinteger(L, n);  // result is integer
  else
    lhat_pushnumber(L, d);  // result is float
}


static int math_floor (lhat_State *L) {
  if (lhat_isinteger(L, 1))
    lhat_settop(L, 1);  // integer is its own floor
  else {
    lhat_Number d = l_mathop(floor)(lhatL_checknumber(L, 1));
    pushnumint(L, d);
  }
  return 1;
}


static int math_ceil (lhat_State *L) {
  if (lhat_isinteger(L, 1))
    lhat_settop(L, 1);  // integer is its own ceil
  else {
    lhat_Number d = l_mathop(ceil)(lhatL_checknumber(L, 1));
    pushnumint(L, d);
  }
  return 1;
}


static int math_fmod (lhat_State *L) {
  if (lhat_isinteger(L, 1) && lhat_isinteger(L, 2)) {
    lhat_Integer d = lhat_tointeger(L, 2);
    if ((lhat_Unsigned)d + 1u <= 1u) {  // special cases: -1 or 0
      lhatL_argcheck(L, d != 0, 2, "zero");
      lhat_pushinteger(L, 0);  // avoid overflow with 0x80000... / -1
    }
    else
      lhat_pushinteger(L, lhat_tointeger(L, 1) % d);
  }
  else
    lhat_pushnumber(L, l_mathop(fmod)(lhatL_checknumber(L, 1),
                                     lhatL_checknumber(L, 2)));
  return 1;
}


//
// next function does not use 'modf', avoiding problems with 'double*'
// (which is not compatible with 'float*') when lhat_Number is not
// 'double'.
//
static int math_modf (lhat_State *L) {
  if (lhat_isinteger(L ,1)) {
    lhat_settop(L, 1);  // number is its own integer part
    lhat_pushnumber(L, 0);  // no fractional part
  }
  else {
    lhat_Number n = lhatL_checknumber(L, 1);
    // integer part (rounds toward zero)
    lhat_Number ip = (n < 0) ? l_mathop(ceil)(n) : l_mathop(floor)(n);
    pushnumint(L, ip);
    // fractional part (test needed for inf/-inf)
    lhat_pushnumber(L, (n == ip) ? l_mathop(0.0) : (n - ip));
  }
  return 2;
}


static int math_sqrt (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(sqrt)(lhatL_checknumber(L, 1)));
  return 1;
}


static int math_ult (lhat_State *L) {
  lhat_Integer a = lhatL_checkinteger(L, 1);
  lhat_Integer b = lhatL_checkinteger(L, 2);
  lhat_pushboolean(L, (lhat_Unsigned)a < (lhat_Unsigned)b);
  return 1;
}

static int math_log (lhat_State *L) {
  lhat_Number x = lhatL_checknumber(L, 1);
  lhat_Number res;
  if (lhat_isnoneornil(L, 2))
    res = l_mathop(log)(x);
  else {
    lhat_Number base = lhatL_checknumber(L, 2);
#if !defined(LHAT_USE_C89)
    if (base == l_mathop(2.0))
      res = l_mathop(log2)(x); else
#endif
    if (base == l_mathop(10.0))
      res = l_mathop(log10)(x);
    else
      res = l_mathop(log)(x)/l_mathop(log)(base);
  }
  lhat_pushnumber(L, res);
  return 1;
}

static int math_exp (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(exp)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_deg (lhat_State *L) {
  lhat_pushnumber(L, lhatL_checknumber(L, 1) * (l_mathop(180.0) / PI));
  return 1;
}

static int math_rad (lhat_State *L) {
  lhat_pushnumber(L, lhatL_checknumber(L, 1) * (PI / l_mathop(180.0)));
  return 1;
}


static int math_min (lhat_State *L) {
  int n = lhat_gettop(L);  // number of arguments
  int imin = 1;  // index of current minimum value
  int i;
  lhatL_argcheck(L, n >= 1, 1, "value expected");
  for (i = 2; i <= n; i++) {
    if (lhat_compare(L, i, imin, LHAT_OPLT))
      imin = i;
  }
  lhat_pushvalue(L, imin);
  return 1;
}


static int math_max (lhat_State *L) {
  int n = lhat_gettop(L);  // number of arguments
  int imax = 1;  // index of current maximum value
  int i;
  lhatL_argcheck(L, n >= 1, 1, "value expected");
  for (i = 2; i <= n; i++) {
    if (lhat_compare(L, imax, i, LHAT_OPLT))
      imax = i;
  }
  lhat_pushvalue(L, imax);
  return 1;
}

//
// This function uses 'double' (instead of 'lhat_Number') to ensure that
// all bits from 'l_rand' can be represented, and that 'RANDMAX + 1.0'
// will keep full precision (ensuring that 'r' is always less than 1.0.)
//
static int math_random (lhat_State *L) {
  lhat_Integer low, up;
  double r = (double)l_rand() * (1.0 / ((double)L_RANDMAX + 1.0));
  switch (lhat_gettop(L)) {  // check number of arguments
    case 0: {  // no arguments
      lhat_pushnumber(L, (lhat_Number)r);  // Number between 0 and 1
      return 1;
    }
    case 1: {  // only upper limit
      low = 1;
      up = lhatL_checkinteger(L, 1);
      break;
    }
    case 2: {  // lower and upper limits
      low = lhatL_checkinteger(L, 1);
      up = lhatL_checkinteger(L, 2);
      break;
    }
    default: return lhatL_error(L, "wrong number of arguments");
  }
  // random integer in the interval [low, up]
  lhatL_argcheck(L, low <= up, 1, "interval is empty");
  lhatL_argcheck(L, low >= 0 || up <= LHAT_MAXINTEGER + low, 1,
                   "interval too large");
  r *= (double)(up - low) + 1.0;
  lhat_pushinteger(L, (lhat_Integer)r + low);
  return 1;
}


static int math_randomseed (lhat_State *L) {
  l_srand((unsigned int)(lhat_Integer)lhatL_checknumber(L, 1));
  (void)l_rand(); // discard first value to avoid undesirable correlations
  return 0;
}


static int math_type (lhat_State *L) {
  if (lhat_type(L, 1) == LHAT_TNUMBER) {
      if (lhat_isinteger(L, 1))
        lhat_pushliteral(L, "integer");
      else
        lhat_pushliteral(L, "float");
  }
  else {
    lhatL_checkany(L, 1);
    lhat_pushnil(L);
  }
  return 1;
}


//
// {==================================================================
// Deprecated functions (for compatibility only)
// ===================================================================
//
#if defined(LHAT_COMPAT_MATHLIB)

static int math_cosh (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(cosh)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_sinh (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(sinh)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_tanh (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(tanh)(lhatL_checknumber(L, 1)));
  return 1;
}

static int math_pow (lhat_State *L) {
  lhat_Number x = lhatL_checknumber(L, 1);
  lhat_Number y = lhatL_checknumber(L, 2);
  lhat_pushnumber(L, l_mathop(pow)(x, y));
  return 1;
}

static int math_frexp (lhat_State *L) {
  int e;
  lhat_pushnumber(L, l_mathop(frexp)(lhatL_checknumber(L, 1), &e));
  lhat_pushinteger(L, e);
  return 2;
}

static int math_ldexp (lhat_State *L) {
  lhat_Number x = lhatL_checknumber(L, 1);
  int ep = (int)lhatL_checkinteger(L, 2);
  lhat_pushnumber(L, l_mathop(ldexp)(x, ep));
  return 1;
}

static int math_log10 (lhat_State *L) {
  lhat_pushnumber(L, l_mathop(log10)(lhatL_checknumber(L, 1)));
  return 1;
}

#endif
// }==================================================================



static const lhatL_Reg mathlib[] = {
  {"abs",   math_abs},
  {"acos",  math_acos},
  {"asin",  math_asin},
  {"atan",  math_atan},
  {"ceil",  math_ceil},
  {"cos",   math_cos},
  {"deg",   math_deg},
  {"exp",   math_exp},
  {"tointeger", math_toint},
  {"floor", math_floor},
  {"fmod",   math_fmod},
  {"ult",   math_ult},
  {"log",   math_log},
  {"max",   math_max},
  {"min",   math_min},
  {"modf",   math_modf},
  {"rad",   math_rad},
  {"random",     math_random},
  {"randomseed", math_randomseed},
  {"sin",   math_sin},
  {"sqrt",  math_sqrt},
  {"tan",   math_tan},
  {"type", math_type},
#if defined(LHAT_COMPAT_MATHLIB)
  {"atan2", math_atan},
  {"cosh",   math_cosh},
  {"sinh",   math_sinh},
  {"tanh",   math_tanh},
  {"pow",   math_pow},
  {"frexp", math_frexp},
  {"ldexp", math_ldexp},
  {"log10", math_log10},
#endif
  // placeholders
  {"pi", NULL},
  {"huge", NULL},
  {"maxinteger", NULL},
  {"mininteger", NULL},
  {NULL, NULL}
};


//
// Open math library
//
LHATMOD_API int lhatopen_math (lhat_State *L) {
  lhatL_newlib(L, mathlib);
  lhat_pushnumber(L, PI);
  lhat_setfield(L, -2, "pi");
  lhat_pushnumber(L, (lhat_Number)HUGE_VAL);
  lhat_setfield(L, -2, "huge");
  lhat_pushinteger(L, LHAT_MAXINTEGER);
  lhat_setfield(L, -2, "maxinteger");
  lhat_pushinteger(L, LHAT_MININTEGER);
  lhat_setfield(L, -2, "mininteger");
  return 1;
}

