//
// $Id: linit.c,v 1.39 2016/12/04 20:17:24 roberto Exp $
// Initialization of libraries for lhat.c and other clients
// See Copyright Notice in lhat.h
//


#define linit_c
#define LHAT_LIB

//
// If you embed Lhat in your program and need to open the standard
// libraries, call lhatL_openlibs in your program. If you need a
// different set of libraries, copy this file to your project and edit
// it to suit your needs.
//
// You can also *preload* libraries, so that a later 'require' can
// open the library, which is already linked to the application.
// For that, do the following code:
//
//  lhatL_getsubtable(L, LHAT_REGISTRYINDEX, LHAT_PRELOAD_TABLE);
//  lhat_pushcfunction(L, lhatopen_modname);
//  lhat_setfield(L, -2, modname);
//  lhat_pop(L, 1);  // remove PRELOAD table
//

#include "lprefix.h"


#include <stddef.h>

#include "lhat.h"

#include "lhatlib.h"
#include "lauxlib.h"


//
// these libs are loaded by lhat.c and are readily available to any Lhat
// program
//
static const lhatL_Reg loadedlibs[] = {
  {"_G", lhatopen_base},
  {LHAT_LOADLIBNAME, lhatopen_package},
  {LHAT_COLIBNAME, lhatopen_coroutine},
  {LHAT_TABLIBNAME, lhatopen_table},
  {LHAT_IOLIBNAME, lhatopen_io},
  {LHAT_OSLIBNAME, lhatopen_os},
  {LHAT_STRLIBNAME, lhatopen_string},
  {LHAT_MATHLIBNAME, lhatopen_math},
  {LHAT_UTF8LIBNAME, lhatopen_utf8},
  {LHAT_DBLIBNAME, lhatopen_debug},
#if defined(LHAT_COMPAT_BITLIB)
  {LHAT_BITLIBNAME, lhatopen_bit32},
#endif
  {NULL, NULL}
};


LHATLIB_API void lhatL_openlibs (lhat_State *L) {
  const lhatL_Reg *lib;
  // "require" functions from 'loadedlibs' and set results to global table
  for (lib = loadedlibs; lib->func; lib++) {
    lhatL_requiref(L, lib->name, lib->func, 1);
    lhat_pop(L, 1);  // remove lib
  }
}

