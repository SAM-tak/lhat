#ifndef lhatlib_h
#define lhatlib_h
//
// L^ standard libraries
// See Copyright Notice in lhat.h
//

#include "lhat.h"

// version suffix for environment variable names
#define LHAT_VERSUFFIX          "_" LHAT_VERSION_MAJOR_ "_" LHAT_VERSION_MINOR_


LHATMOD_API int lhatopen_base(lhat_State *L);

#define LHAT_COLIBNAME	"coroutine"
LHATMOD_API int lhatopen_coroutine(lhat_State *L);

#define LHAT_TABLIBNAME	"table"
LHATMOD_API int lhatopen_table(lhat_State *L);

#define LHAT_IOLIBNAME	"io"
LHATMOD_API int lhatopen_io(lhat_State *L);

#define LHAT_OSLIBNAME	"os"
LHATMOD_API int lhatopen_os(lhat_State *L);

#define LHAT_STRLIBNAME	"string"
LHATMOD_API int lhatopen_string(lhat_State *L);

#define LHAT_UTF8LIBNAME	"utf8"
LHATMOD_API int lhatopen_utf8(lhat_State *L);

#define LHAT_MATHLIBNAME	"math"
LHATMOD_API int lhatopen_math(lhat_State *L);

#define LHAT_DBLIBNAME	"debug"
LHATMOD_API int lhatopen_debug(lhat_State *L);

#define LHAT_LOADLIBNAME	"package"
LHATMOD_API int lhatopen_package(lhat_State *L);


// open all previous libraries
LHATLIB_API void lhatL_openlibs(lhat_State *L);

#if !defined(lhat_assert)
# define lhat_assert(x)	((void)0)
#endif

#endif