#ifndef lhat_lauxlib_h
#define lhat_lauxlib_h
//
// Auxiliary functions for building L^ libraries
// See Copyright Notice in lhat.h
//

#include <stddef.h>
#include <stdio.h>

#include "lhat.h"

enum {
	// extra error code for 'lhatL_loadfilex'
	LHAT_ERRFILE = LHAT_ERRERR + 1,

	LHATL_NUMSIZES = sizeof(lhat_Integer) * 16 + sizeof(lhat_Number),

	// predefined references
	LHAT_NOREF   = -2,
	LHAT_REFNIL  = -1,
};

// key, in the registry, for table of loaded modules
#define LHAT_LOADED_TABLE	"_LOADED"

// key, in the registry, for table of preloaded loaders
#define LHAT_PRELOAD_TABLE	"_PRELOAD"


typedef struct lhatL_Reg {
	const char *name;
	lhat_CFunction func;
} lhatL_Reg;

LHATLIB_API void lhatL_checkversion_(lhat_State *L, lhat_Number ver, size_t sz);
inline void lhatL_checkversion(lhat_State *L)
{
	lhatL_checkversion_(L, LHAT_VERSION_NUM, LHATL_NUMSIZES);
}

LHATLIB_API int lhatL_getmetafield(lhat_State *L, int obj, const char *e);
LHATLIB_API int lhatL_callmeta(lhat_State *L, int obj, const char *e);
LHATLIB_API const char *lhatL_tolstring(lhat_State *L, int idx, size_t *len);
LHATLIB_API int lhatL_argerror(lhat_State *L, int arg, const char *extramsg);
LHATLIB_API const char *lhatL_checklstring(lhat_State *L, int arg, size_t *l);
LHATLIB_API const char *lhatL_optlstring(lhat_State *L, int arg, const char *def, size_t *l);
LHATLIB_API lhat_Number lhatL_checknumber(lhat_State *L, int arg);
LHATLIB_API lhat_Number lhatL_optnumber(lhat_State *L, int arg, lhat_Number def);

LHATLIB_API lhat_Integer lhatL_checkinteger(lhat_State *L, int arg);
LHATLIB_API lhat_Integer lhatL_optinteger(lhat_State *L, int arg, lhat_Integer def);

LHATLIB_API void lhatL_checkstack(lhat_State *L, int sz, const char *msg);
LHATLIB_API void lhatL_checktype(lhat_State *L, int arg, int t);
LHATLIB_API void lhatL_checkany(lhat_State *L, int arg);

LHATLIB_API int lhatL_newmetatable(lhat_State *L, const char *tname);
LHATLIB_API void lhatL_setmetatable(lhat_State *L, const char *tname);
LHATLIB_API void *lhatL_testudata(lhat_State *L, int ud, const char *tname);
LHATLIB_API void *lhatL_checkudata(lhat_State *L, int ud, const char *tname);

LHATLIB_API void lhatL_where(lhat_State *L, int lvl);
LHATLIB_API int lhatL_error(lhat_State *L, const char *fmt, ...);

LHATLIB_API int lhatL_checkoption(lhat_State *L, int arg, const char *def, const char *const lst[]);

LHATLIB_API int lhatL_fileresult(lhat_State *L, int stat, const char *fname);
LHATLIB_API int lhatL_execresult(lhat_State *L, int stat);

LHATLIB_API int lhatL_ref(lhat_State *L, int t);
LHATLIB_API void lhatL_unref(lhat_State *L, int t, int ref);

LHATLIB_API int lhatL_loadfilex(lhat_State *L, const char *filename, const char *mode);

inline int lhatL_loadfile(lhat_State *L, const char *filename)
{
	return lhatL_loadfilex(L, filename, NULL);
}

LHATLIB_API int lhatL_loadbufferx(lhat_State *L, const char *buff, size_t sz, const char *name, const char *mode);

inline  int lhatL_loadbuffer(lhat_State *L, const char *buff, size_t sz, const char *name)
{
	return lhatL_loadbufferx(L, buff, sz, name, NULL);
}

LHATLIB_API int lhatL_loadstring(lhat_State *L, const char *s);

LHATLIB_API lhat_State *lhatL_newstate(void);

LHATLIB_API lhat_Integer lhatL_len(lhat_State *L, int idx);

LHATLIB_API const char *lhatL_gsub(lhat_State *L, const char *s, const char *p, const char *r);

LHATLIB_API void lhatL_setfuncs(lhat_State *L, const lhatL_Reg *l, int nup);

LHATLIB_API int lhatL_getsubtable(lhat_State *L, int idx, const char *fname);

LHATLIB_API void lhatL_traceback(lhat_State *L, lhat_State *L1, const char *msg, int level);

LHATLIB_API void lhatL_requiref(lhat_State *L, const char *modname, lhat_CFunction openf, int glb);

inline const char *lhatL_typename(lhat_State *L, int i)
{
	return lhat_typename(L, lhat_type(L, i));
}

inline const char *lhatL_checkstring(lhat_State *L, int n)
{
	return lhatL_checklstring(L, n, NULL);
}

inline const char *lhatL_optstring(lhat_State *L, int n, const char *def)
{
	return lhatL_optlstring(L, n, def, NULL);
}

inline int lhatL_getmetatable(lhat_State *L, const char *k)
{
	return lhat_getfield(L, LHAT_REGISTRYINDEX, k);
}

inline int lhatL_dofile(lhat_State *L, const char *fn)
{
	return (lhatL_loadfile(L, fn) || lhat_pcall(L, 0, LHAT_MULTRET, 0));
}

inline int lhatL_dostring(lhat_State *L, const char *s)
{
	return (lhatL_loadstring(L, s) || lhat_pcall(L, 0, LHAT_MULTRET, 0));
}

//
// ===============================================================
// some useful macros
// ===============================================================
//


#define lhatL_newlibtable(L,l)	\
	lhat_createtable(L, 0, sizeof(l)/sizeof((l)[0]) - 1)

#define lhatL_newlib(L,l)  \
	(lhatL_checkversion(L), lhatL_newlibtable(L,l), lhatL_setfuncs(L,l,0))

#define lhatL_argcheck(L, cond,arg,extramsg)	\
	((void)((cond) || lhatL_argerror(L, (arg), (extramsg))))

#define lhatL_opt(L,f,n,d)	(lhat_isnoneornil(L,(n)) ? (d) : f(L,(n)))


//
// {======================================================
// Generic Buffer manipulation
// =======================================================
//

typedef struct lhatL_Buffer {
	char *b;  // buffer address
	size_t size;  // buffer size
	size_t n;  // number of characters in buffer
	lhat_State *L;
	char initb[LHATL_BUFFERSIZE];  // initial buffer
} lhatL_Buffer;

LHATLIB_API void lhatL_buffinit(lhatL_Buffer *B, lhat_State *L);
LHATLIB_API char *lhatL_prepbuffsize(lhatL_Buffer *B, size_t sz);
LHATLIB_API void lhatL_addlstring(lhatL_Buffer *B, const char *s, size_t l);
LHATLIB_API void lhatL_addstring(lhatL_Buffer *B, const char *s);
LHATLIB_API void lhatL_addvalue(lhatL_Buffer *B);
LHATLIB_API void lhatL_pushresult(lhatL_Buffer *B);
LHATLIB_API void lhatL_pushresultsize(lhatL_Buffer *B, size_t sz);
LHATLIB_API char *lhatL_buffinitsize(lhatL_Buffer *B, size_t sz, lhat_State *L);

inline void lhatL_addchar(lhatL_Buffer *B, char c)
{
	if(B->n >= B->size) lhatL_prepbuffsize(B, 1);
	B->b[B->n++] = c;
}

inline void lhatL_addsize(lhatL_Buffer *B, size_t s)
{
	B->n += s;
}

inline char *lhatL_prepbuffer(lhatL_Buffer *B)
{
	return lhatL_prepbuffsize(B, LHATL_BUFFERSIZE);
}

// }======================================================



//
// {======================================================
// File handles for IO library
// =======================================================
//

//
// A file handle is a userdata with metatable 'LHAT_FILEHANDLE' and
// initial structure 'lhatL_Stream' (it may contain other fields
// after that initial structure).
//

#define LHAT_FILEHANDLE          "FILE*"


typedef struct lhatL_Stream {
	FILE *f;  // stream (NULL for incompletely created streams)
	lhat_CFunction closef;  // to close stream (NULL for closed streams)
} lhatL_Stream;

// }======================================================



//
// {==================================================================
// "Abstraction Layer" for basic report of messages and errors
// ===================================================================
//

// print a string
#if !defined(lhat_writestring)
inline size_t lhat_writestring(void const *s, size_t l)
{
	return fwrite(s, sizeof(char), l, stdout);
}
#endif

// print a newline and flush the output
#if !defined(lhat_writeline)
inline int lhat_writeline()
{
	lhat_writestring("\n", 1);
	return fflush(stdout);
}
#endif

// print an error message
#if !defined(lhat_writestringerror)
inline int lhat_writestringerror(char * const s, const char * const p)
{
	fprintf(stderr, s, p);
	return fflush(stderr);
}
#endif

// }==================================================================

#endif // !lhat_lauxlib_h