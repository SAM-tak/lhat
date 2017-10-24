#ifndef lhat_h
#define lhat_h
//
// L^ - A Scripting Language based on Lua
// Osamu Takasugi & Lua.org, PUC-Rio, Brazil (http://www.lhat.org)
// See Copyright Notice at the end of this file
//

#include <stdarg.h>
#include <stddef.h>

#include "lhatconf.h"

#define LHAT_VERSION_MAJOR    0
#define LHAT_VERSION_MINOR    1
#define LHAT_VERSION_RELEASE  0

#define LHAT_STRMACRO(s)      #s
#define LHAT_STRSTRMACRO(s)   LHAT_STRMACRO(s)
#define LHAT_VERSION_MAJOR_   LHAT_STRSTRMACRO(LHAT_VERSION_MAJOR)
#define LHAT_VERSION_MINOR_   LHAT_STRSTRMACRO(LHAT_VERSION_MINOR)
#define LHAT_VERSION_RELEASE_ LHAT_STRSTRMACRO(LHAT_VERSION_RELEASE)

enum {
	LHAT_VERSION_NUM = (LHAT_VERSION_MAJOR << 12) | (LHAT_VERSION_MINOR << 8) | (LHAT_VERSION_RELEASE)
};

#define LHAT_VERSION "L^ " LHAT_VERSION_MAJOR_ "." LHAT_VERSION_MINOR_
#define LHAT_RELEASE LHAT_VERSION "." LHAT_VERSION_RELEASE_
#define LHAT_COPYRIGHT LHAT_RELEASE "  Copyright (C) 2017 Osamu Takasugi, Lua.org, PUC-Rio"

// mark for precompiled code ('<esc><esc>L^')
#define LHAT_SIGNATURE	"\x1b\x1bL^"

// option for multiple returns in 'lhat_pcall' and 'lhat_call'
enum {
	LHAT_MULTRET = -1
};

//
// Pseudo-indices
// (-LHATI_MAXSTACK is the minimum valid index; we keep some free empty
// space after that to help overflow detection)
//
enum {
	LHAT_REGISTRYINDEX = -LHATI_MAXSTACK - 1000
};

inline int lhat_upvalueindex(int i)
{
	return LHAT_REGISTRYINDEX - i;
}


// coroutine status
enum {
	LHAT_OK,        // 0
	LHAT_YIELD,     // 1
	LHAT_ERRRUN,    // 2
	LHAT_ERRSYNTAX, // 3
	LHAT_ERRMEM,    // 4
	LHAT_ERRGCMM,   // 5
	LHAT_ERRERR,    // 6
};

typedef struct lhat_State lhat_State;


//
// basic types
//
enum {
	LHAT_TNIL,           // 0
	LHAT_TBOOLEAN,       // 1
	LHAT_TLIGHTUSERDATA, // 2
	LHAT_TNUMBER,        // 3
	LHAT_TSTRING,        // 4
	LHAT_TTABLE,         // 5
	LHAT_TFUNCTION,      // 6
	LHAT_TUSERDATA,      // 7
	LHAT_TCOROUTINE,     // 8
	LHAT_NUMTAGS,        // 9

	LHAT_TNONE = -1
};


// minimum L^ stack available to a C function
enum {
	LHAT_MINSTACK = 20
};


// predefined values in the registry
enum {
	LHAT_RIDX_MAINSTATE = 1,
	LHAT_RIDX_GLOBALS = 2,
	LHAT_RIDX_LAST = LHAT_RIDX_GLOBALS
};


// type of numbers in L^
typedef LHAT_NUMBER lhat_Number;


// type for integer functions
typedef LHAT_INTEGER lhat_Integer;

// unsigned integer type
typedef LHAT_UNSIGNED lhat_Unsigned;

// type for continuation-function contexts
typedef LHAT_KCONTEXT lhat_KContext;


//
// Type for C functions registered with L^
//
typedef int (*lhat_CFunction)(lhat_State *L);

//
// Type for continuation functions
//
typedef int (*lhat_KFunction)(lhat_State *L, int status, lhat_KContext ctx);


//
// Type for functions that read/write blocks when loading/dumping L^ chunks
//
typedef const char *(*lhat_Reader)(lhat_State *L, void *ud, size_t *sz);

typedef int (*lhat_Writer)(lhat_State *L, const void *p, size_t sz, void *ud);


//
// Type for memory-allocation functions
//
typedef void *(*lhat_Alloc)(void *ud, void *ptr, size_t osize, size_t nsize);



//
// generic extra include file
//
#if defined(LHAT_USER_H)
# include LHAT_USER_H
#endif


//
// state manipulation
//
LHAT_API lhat_State *lhat_newstate(lhat_Alloc f, void *ud);
LHAT_API void       lhat_close(lhat_State *L);
LHAT_API lhat_State *lhat_newcoroutine(lhat_State *L);

LHAT_API lhat_CFunction lhat_atpanic(lhat_State *L, lhat_CFunction panicf);


LHAT_API const lhat_Number *lhat_version(lhat_State *L);


//
// basic stack manipulation
//
LHAT_API int   (lhat_absindex)(lhat_State *L, int idx);
LHAT_API int   (lhat_gettop)(lhat_State *L);
LHAT_API void  (lhat_settop)(lhat_State *L, int idx);
LHAT_API void  (lhat_pushvalue)(lhat_State *L, int idx);
LHAT_API void  (lhat_rotate)(lhat_State *L, int idx, int n);
LHAT_API void  (lhat_copy)(lhat_State *L, int fromidx, int toidx);
LHAT_API int   (lhat_checkstack)(lhat_State *L, int n);

LHAT_API void  (lhat_xmove)(lhat_State *from, lhat_State *to, int n);


//
// access functions (stack -> C)
//

LHAT_API int             (lhat_isnumber)(lhat_State *L, int idx);
LHAT_API int             (lhat_isstring)(lhat_State *L, int idx);
LHAT_API int             (lhat_iscfunction)(lhat_State *L, int idx);
LHAT_API int             (lhat_isinteger)(lhat_State *L, int idx);
LHAT_API int             (lhat_isuserdata)(lhat_State *L, int idx);
LHAT_API int             (lhat_type)(lhat_State *L, int idx);
LHAT_API const char     *(lhat_typename)(lhat_State *L, int tp);

LHAT_API lhat_Number      lhat_tonumberx(lhat_State *L, int idx, int *isnum);
LHAT_API lhat_Integer     lhat_tointegerx(lhat_State *L, int idx, int *isnum);
LHAT_API int             lhat_toboolean(lhat_State *L, int idx);
LHAT_API const char     *lhat_tolstring(lhat_State *L, int idx, size_t *len);
LHAT_API size_t          lhat_rawlen(lhat_State *L, int idx);
LHAT_API lhat_CFunction   lhat_tocfunction(lhat_State *L, int idx);
LHAT_API void	       *lhat_touserdata(lhat_State *L, int idx);
LHAT_API lhat_State      *lhat_tocoroutine(lhat_State *L, int idx);
LHAT_API const void     *lhat_topointer(lhat_State *L, int idx);


//
// Comparison and arithmetic functions
//
enum {
	LHAT_OPADD,  // 0	// ORDER MM, ORDER OP
	LHAT_OPSUB,  // 1
	LHAT_OPMUL,  // 2
	LHAT_OPMOD,  // 3
	LHAT_OPPOW,  // 4
	LHAT_OPDIV,  // 5
	LHAT_OPIDIV, // 6
	LHAT_OPBAND, // 7
	LHAT_OPBOR,  // 8
	LHAT_OPBXOR, // 9
	LHAT_OPSHL,  // 10
	LHAT_OPSHR,  // 11
	LHAT_OPUNM,  // 12
	LHAT_OPBNOT, // 13
};

LHAT_API void  (lhat_arith)(lhat_State *L, int op);

enum {
	LHAT_OPEQ, // 0
	LHAT_OPLT, // 1
	LHAT_OPLE, // 2
};

LHAT_API int   (lhat_rawequal)(lhat_State *L, int idx1, int idx2);
LHAT_API int   (lhat_compare)(lhat_State *L, int idx1, int idx2, int op);


//
// push functions (C -> stack)
//
LHAT_API void        (lhat_pushnil)(lhat_State *L);
LHAT_API void        (lhat_pushnumber)(lhat_State *L, lhat_Number n);
LHAT_API void        (lhat_pushinteger)(lhat_State *L, lhat_Integer n);
LHAT_API const char *(lhat_pushlstring)(lhat_State *L, const char *s, size_t len);
LHAT_API const char *(lhat_pushstring)(lhat_State *L, const char *s);
LHAT_API const char *(lhat_pushvfstring)(lhat_State *L, const char *fmt,
	va_list argp);
LHAT_API const char *(lhat_pushfstring)(lhat_State *L, const char *fmt, ...);
LHAT_API void  (lhat_pushcclosure)(lhat_State *L, lhat_CFunction fn, int n);
LHAT_API void  (lhat_pushboolean)(lhat_State *L, int b);
LHAT_API void  (lhat_pushlightuserdata)(lhat_State *L, void *p);
LHAT_API int   (lhat_pushcoroutine)(lhat_State *L);


//
// get functions (Lhat -> stack)
//
LHAT_API int (lhat_getglobal)(lhat_State *L, const char *name);
LHAT_API int (lhat_gettable)(lhat_State *L, int idx);
LHAT_API int (lhat_getfield)(lhat_State *L, int idx, const char *k);
LHAT_API int (lhat_geti)(lhat_State *L, int idx, lhat_Integer n);
LHAT_API int (lhat_rawget)(lhat_State *L, int idx);
LHAT_API int (lhat_rawgeti)(lhat_State *L, int idx, lhat_Integer n);
LHAT_API int (lhat_rawgetp)(lhat_State *L, int idx, const void *p);

LHAT_API void  (lhat_createtable)(lhat_State *L, int narr, int nrec);
LHAT_API void *(lhat_newuserdata)(lhat_State *L, size_t sz);
LHAT_API int   (lhat_getmetatable)(lhat_State *L, int objindex);
LHAT_API int  (lhat_getuservalue)(lhat_State *L, int idx);


//
// set functions (stack -> L^)
//
LHAT_API void  (lhat_setglobal)(lhat_State *L, const char *name);
LHAT_API void  (lhat_settable)(lhat_State *L, int idx);
LHAT_API void  (lhat_setfield)(lhat_State *L, int idx, const char *k);
LHAT_API void  (lhat_seti)(lhat_State *L, int idx, lhat_Integer n);
LHAT_API void  (lhat_rawset)(lhat_State *L, int idx);
LHAT_API void  (lhat_rawseti)(lhat_State *L, int idx, lhat_Integer n);
LHAT_API void  (lhat_rawsetp)(lhat_State *L, int idx, const void *p);
LHAT_API int   (lhat_setmetatable)(lhat_State *L, int objindex);
LHAT_API void  (lhat_setuservalue)(lhat_State *L, int idx);


//
// 'load' and 'call' functions (load and run L^ code)
//
LHAT_API void  (lhat_callk)(lhat_State *L, int nargs, int nresults,
	lhat_KContext ctx, lhat_KFunction k);
#define lhat_call(L,n,r)		lhat_callk(L, (n), (r), 0, NULL)

LHAT_API int   (lhat_pcallk)(lhat_State *L, int nargs, int nresults, int errfunc,
	lhat_KContext ctx, lhat_KFunction k);
#define lhat_pcall(L,n,r,f)	lhat_pcallk(L, (n), (r), (f), 0, NULL)

LHAT_API int   (lhat_load)(lhat_State *L, lhat_Reader reader, void *dt,
	const char *chunkname, const char *mode);

LHAT_API int (lhat_dump)(lhat_State *L, lhat_Writer writer, void *data, int strip);


//
// coroutine functions
//
LHAT_API int  (lhat_yieldk)(lhat_State *L, int nresults, lhat_KContext ctx, lhat_KFunction k);
LHAT_API int  (lhat_resume)(lhat_State *L, lhat_State *from, int narg);
LHAT_API int  (lhat_status)(lhat_State *L);
LHAT_API int (lhat_isyieldable)(lhat_State *L);

#define lhat_yield(L,n)		lhat_yieldk(L, (n), 0, NULL)


//
// garbage-collection function and options
//
enum {
	LHAT_GCSTOP,          // 0
	LHAT_GCRESTART,       // 1
	LHAT_GCCOLLECT,       // 2
	LHAT_GCCOUNT,         // 3
	LHAT_GCCOUNTB,        // 4
	LHAT_GCSTEP,          // 5
	LHAT_GCSETPAUSE,      // 6
	LHAT_GCSETSTEPMUL,    // 7
	LHAT_GCISRUNNING,     // 8
};

LHAT_API int lhat_gc(lhat_State *L, int what, int data);


//
// miscellaneous functions
//

LHAT_API int   (lhat_error)(lhat_State *L);

LHAT_API int   (lhat_next)(lhat_State *L, int idx);

LHAT_API void  (lhat_concat)(lhat_State *L, int n);
LHAT_API void  (lhat_len)(lhat_State *L, int idx);

LHAT_API size_t   lhat_stringtonumber(lhat_State *L, const char *s);

LHAT_API lhat_Alloc lhat_getallocf(lhat_State *L, void **ud);
LHAT_API void      lhat_setallocf(lhat_State *L, lhat_Alloc f, void *ud);



//
// {==============================================================
// some useful macros
// ===============================================================
//

#define lhat_getextraspace(L)	((void *)((char *)(L) - LHAT_EXTRASPACE))

#define lhat_tonumber(L,i)	lhat_tonumberx(L,(i),NULL)
#define lhat_tointeger(L,i)	lhat_tointegerx(L,(i),NULL)

#define lhat_pop(L,n)		lhat_settop(L, -(n)-1)

#define lhat_newtable(L)		lhat_createtable(L, 0, 0)

#define lhat_register(L,n,f) (lhat_pushcfunction(L, (f)), lhat_setglobal(L, (n)))

#define lhat_pushcfunction(L,f)	lhat_pushcclosure(L, (f), 0)

#define lhat_isfunction(L,n)	(lhat_type(L, (n)) == LHAT_TFUNCTION)
#define lhat_istable(L,n)	(lhat_type(L, (n)) == LHAT_TTABLE)
#define lhat_islightuserdata(L,n)	(lhat_type(L, (n)) == LHAT_TLIGHTUSERDATA)
#define lhat_isnil(L,n)		(lhat_type(L, (n)) == LHAT_TNIL)
#define lhat_isboolean(L,n)	(lhat_type(L, (n)) == LHAT_TBOOLEAN)
#define lhat_iscoroutine(L,n)	(lhat_type(L, (n)) == LHAT_TCOROUTINE)
#define lhat_isnone(L,n)		(lhat_type(L, (n)) == LHAT_TNONE)
#define lhat_isnoneornil(L, n)	(lhat_type(L, (n)) <= 0)

#define lhat_pushliteral(L, s)	lhat_pushstring(L, "" s)

#define lhat_pushglobaltable(L)  \
	((void)lhat_rawgeti(L, LHAT_REGISTRYINDEX, LHAT_RIDX_GLOBALS))

#define lhat_tostring(L,i)	lhat_tolstring(L, (i), NULL)


#define lhat_insert(L,idx)	lhat_rotate(L, (idx), 1)

#define lhat_remove(L,idx)	(lhat_rotate(L, (idx), -1), lhat_pop(L, 1))

#define lhat_replace(L,idx)	(lhat_copy(L, -1, (idx)), lhat_pop(L, 1))

// }==============================================================


//
// {======================================================================
// Debug API
// =======================================================================
//


//
// Event codes & Event masks
//
enum {
	LHAT_HOOKCALL,	   // 0
	LHAT_HOOKRET,	   // 1
	LHAT_HOOKLINE,	   // 2
	LHAT_HOOKCOUNT,	   // 3
	LHAT_HOOKTAILCALL, // 4
	LHAT_MASKCALL = (1 << LHAT_HOOKCALL),
	LHAT_MASKRET = (1 << LHAT_HOOKRET),
	LHAT_MASKLINE = (1 << LHAT_HOOKLINE),
	LHAT_MASKCOUNT = (1 << LHAT_HOOKCOUNT),
};

// activation record
typedef struct lhat_Debug lhat_Debug;  


// Functions to be called by the debugger in specific events
typedef void (*lhat_Hook)(lhat_State *L, lhat_Debug *ar);


LHAT_API int lhat_getstack(lhat_State *L, int level, lhat_Debug *ar);
LHAT_API int lhat_getinfo(lhat_State *L, const char *what, lhat_Debug *ar);
LHAT_API const char *lhat_getlocal(lhat_State *L, const lhat_Debug *ar, int n);
LHAT_API const char *lhat_setlocal(lhat_State *L, const lhat_Debug *ar, int n);
LHAT_API const char *lhat_getupvalue(lhat_State *L, int funcindex, int n);
LHAT_API const char *lhat_setupvalue(lhat_State *L, int funcindex, int n);

LHAT_API void *lhat_upvalueid(lhat_State *L, int fidx, int n);
LHAT_API void lhat_upvaluejoin(lhat_State *L, int fidx1, int n1, int fidx2, int n2);

LHAT_API void lhat_sethook(lhat_State *L, lhat_Hook func, int mask, int count);
LHAT_API lhat_Hook lhat_gethook(lhat_State *L);
LHAT_API int lhat_gethookmask(lhat_State *L);
LHAT_API int lhat_gethookcount(lhat_State *L);


struct lhat_Debug {
	int event;
	const char *name;	// (n)
	const char *namewhat;	// (n) 'global', 'local', 'field', 'method'
	const char *what;	// (S) 'Lhat', 'C', 'main', 'tail'
	const char *source;	// (S)
	int currentline;	// (l)
	int linedefined;	// (S)
	int lastlinedefined;	// (S)
	unsigned char nups;	// (u) number of upvalues
	unsigned char nparams;// (u) number of parameters
	char isvararg;        // (u)
	char istailcall;	// (t)
	char short_src[LHAT_IDSIZE]; // (S)
	// private part
	struct CallInfo *i_ci;  // active function
};

// }======================================================================


/*****************************************************************************
* Copyright (C) 2017 Osamu Takasugi, Lua.org, PUC-Rio.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
***************************************************************************/

#endif