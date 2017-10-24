//
// Dynamic library loader for L^
// See Copyright Notice in lhat.h
//
// This module contains an implementation of loadlib for Unix systems
// that have dlfcn, an implementation for Windows, and a stub for other
// systems.
//

#define LHAT_LIB

#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"


//
// LHAT_IGMARK is a mark to ignore all before it when building the
// lhatopen_ function name.
//
#if !defined (LHAT_IGMARK)
# define LHAT_IGMARK		"-"
#endif


//
// LHAT_CSUBSEP is the character that replaces dots in submodule names
// when searching for a C loader.
// LHAT_LSUBSEP is the character that replaces dots in submodule names
// when searching for a L^ loader.
//
#if !defined(LHAT_CSUBSEP)
# define LHAT_CSUBSEP		LHAT_DIRSEP
#endif

#if !defined(LHAT_LSUBSEP)
# define LHAT_LSUBSEP		LHAT_DIRSEP
#endif


// prefix for open functions in C libraries
#define LHAT_POF		"lhatopen_"

// separator for open functions in C libraries
#define LHAT_OFSEP	"_"


//
// unique key for table in the registry that keeps handles
// for all loaded C libraries
//
static const int CLIBS = 0;

#define LIB_FAIL	"open"


#define setprogdir(L)           ((void)0)


//
// system-dependent functions
//

//
// unload library 'lib'
//
static void lsys_unloadlib(void *lib);

//
// load C library in file 'path'. If 'seeglb', load with all names in
// the library global.
// Returns the library; in case of error, returns NULL plus an
// error string in the stack.
//
static void *lsys_load(lhat_State *L, const char *path, int seeglb);

//
// Try to find a function named 'sym' in library 'lib'.
// Returns the function; in case of error, returns NULL plus an
// error string in the stack.
//
static lhat_CFunction lsys_sym(lhat_State *L, void *lib, const char *sym);




#if defined(LHAT_USE_DLOPEN)
//
// {========================================================================
// This is an implementation of loadlib based on the dlfcn interface.
// The dlfcn interface is available in Linux, SunOS, Solaris, IRIX, FreeBSD,
// NetBSD, AIX 4.2, HPUX 11, and  probably most other Unix flavors, at least
// as an emulation layer on top of native functions.
// =========================================================================
//

# include <dlfcn.h>

//
// Macro to convert pointer-to-void* to pointer-to-function. This cast
// is undefined according to ISO C, but POSIX assumes that it works.
// (The '__extension__' in gnu compilers is only to avoid warnings.)
//
# if defined(__GNUC__)
#  define cast_func(p) (__extension__ (lhat_CFunction)(p))
# else
#  define cast_func(p) ((lhat_CFunction)(p))
# endif

static void lsys_unloadlib(void *lib)
{
    dlclose(lib);
}

static void *lsys_load(lhat_State *L, const char *path, int seeglb)
{
    void *lib = dlopen(path, RTLD_NOW | (seeglb ? RTLD_GLOBAL : RTLD_LOCAL));
    if(lib == NULL) lhat_pushstring(L, dlerror());
    return lib;
}

static lhat_CFunction lsys_sym(lhat_State *L, void *lib, const char *sym)
{
    lhat_CFunction f = cast_func(dlsym(lib, sym));
    if(f == NULL) lhat_pushstring(L, dlerror());
    return f;
}
#elif defined(LHAT_DL_DLL)
//
// {======================================================================
// This is an implementation of loadlib for Windows using native functions.
// =======================================================================
//
# include <windows.h>

//
// optional flags for LoadLibraryEx
//
# if !defined(LHAT_LLE_FLAGS)
#  define LHAT_LLE_FLAGS	0
# endif


# undef setprogdir

//
// Replace in the path (on the top of the stack) any occurrence
// of LHAT_EXEC_DIR with the executable's path.
//
static void setprogdir(lhat_State *L)
{
    char buff[MAX_PATH + 1];
    char *lb;
    DWORD nsize = sizeof(buff) / sizeof(char);
    DWORD n = GetModuleFileNameA(NULL, buff, nsize);  // get exec. name
    if(n == 0 || n == nsize || (lb = strrchr(buff, '\\')) == NULL)
        lhatL_error(L, "unable to get ModuleFileName");
    else {
        *lb = '\0';  // cut name on the last '\\' to get the path
        lhatL_gsub(L, lhat_tostring(L, -1), LHAT_EXEC_DIR, buff);
        lhat_remove(L, -2);  // remove original string
    }
}

static void pusherror(lhat_State *L)
{
    int error = GetLastError();
    char buffer[128];
    if(FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, error, 0, buffer, sizeof(buffer) / sizeof(char), NULL))
        lhat_pushstring(L, buffer);
    else
        lhat_pushfstring(L, "system error %d\n", error);
}

static void lsys_unloadlib(void *lib)
{
    FreeLibrary((HMODULE)lib);
}

static void *lsys_load(lhat_State *L, const char *path, int seeglb)
{
    HMODULE lib = LoadLibraryExA(path, NULL, LHAT_LLE_FLAGS);
    (void)(seeglb);  // not used: symbols are 'global' by default
    if(lib == NULL) pusherror(L);
    return lib;
}

static lhat_CFunction lsys_sym(lhat_State *L, void *lib, const char *sym)
{
    lhat_CFunction f = (lhat_CFunction)GetProcAddress((HMODULE)lib, sym);
    if(f == NULL) pusherror(L);
    return f;
}
#else
//
// {======================================================
// Fallback for other systems
// =======================================================
//
# undef LIB_FAIL
# define LIB_FAIL	"absent"

# define DLMSG	"dynamic libraries not enabled; check your L^ installation"

static void lsys_unloadlib(void *lib)
{
    (void)(lib);  // not used
}

static void *lsys_load(lhat_State *L, const char *path, int seeglb)
{
    (void)(path); (void)(seeglb);  // not used
    lhat_pushliteral(L, DLMSG);
    return NULL;
}

static lhat_CFunction lsys_sym(lhat_State *L, void *lib, const char *sym)
{
    (void)(lib); (void)(sym);  // not used
    lhat_pushliteral(L, DLMSG);
    return NULL;
}
#endif


//
// {==================================================================
// Set Paths
// ===================================================================
//

//
// LHAT_PATH_VAR and LHAT_CPATH_VAR are the names of the environment
// variables that L^ check to set its paths.
//
#if !defined(LHAT_PATH_VAR)
#define LHAT_PATH_VAR    "LHAT_PATH"
#endif

#if !defined(LHAT_CPATH_VAR)
#define LHAT_CPATH_VAR   "LHAT_CPATH"
#endif


#define AUXMARK         "\1"	// auxiliary mark


//
// return registry.LHAT_NOENV as a boolean
//
static int noenv(lhat_State *L)
{
    lhat_getfield(L, LHAT_REGISTRYINDEX, "LHAT_NOENV");
    int b = lhat_toboolean(L, -1);
    lhat_pop(L, 1);  // remove value
    return b;
}


//
// Set a path
//
static void setpath(lhat_State *L, const char *fieldname, const char *envname, const char *dft)
{
    const char *nver = lhat_pushfstring(L, "%s%s", envname, LHAT_VERSUFFIX);
    const char *path = getenv(nver);  // use versioned name
    if(path == NULL)  // no environment variable?
        path = getenv(envname);  // try unversioned name
    if(path == NULL || noenv(L))  // no environment variable?
        lhat_pushstring(L, dft);  // use default
    else {
        // replace ";;" by ";AUXMARK;" and then AUXMARK by default path
        path = lhatL_gsub(L, path, LHAT_PATH_SEP LHAT_PATH_SEP,
            LHAT_PATH_SEP AUXMARK LHAT_PATH_SEP);
        lhatL_gsub(L, path, AUXMARK, dft);
        lhat_remove(L, -2); // remove result from 1st 'gsub'
    }
    setprogdir(L);
    lhat_setfield(L, -3, fieldname);  // package[fieldname] = path value
    lhat_pop(L, 1);  // pop versioned variable name
}

// }==================================================================


//
// return registry.CLIBS[path]
//
static void *checkclib(lhat_State *L, const char *path)
{
    lhat_rawgetp(L, LHAT_REGISTRYINDEX, &CLIBS);
    lhat_getfield(L, -1, path);
    void *plib = lhat_touserdata(L, -1);  // plib = CLIBS[path]
    lhat_pop(L, 2);  // pop CLIBS table and 'plib'
    return plib;
}


//
// registry.CLIBS[path] = plib        -- for queries
// registry.CLIBS[#CLIBS + 1] = plib  -- also keep a list of all libraries
//
static void addtoclib(lhat_State *L, const char *path, void *plib)
{
    lhat_rawgetp(L, LHAT_REGISTRYINDEX, &CLIBS);
    lhat_pushlightuserdata(L, plib);
    lhat_pushvalue(L, -1);
    lhat_setfield(L, -3, path);  // CLIBS[path] = plib
    lhat_rawseti(L, -2, lhatL_len(L, -2) + 1);  // CLIBS[#CLIBS + 1] = plib
    lhat_pop(L, 1);  // pop CLIBS table
}


//
// __gc tag method for CLIBS table: calls 'lsys_unloadlib' for all lib
// handles in list CLIBS
//
static int gctm(lhat_State *L)
{
    for(lhat_Integer n = lhatL_len(L, 1); n >= 1; n--) {  // for each handle, in reverse order
        lhat_rawgeti(L, 1, n);  // get handle CLIBS[n]
        lsys_unloadlib(lhat_touserdata(L, -1));
        lhat_pop(L, 1);  // pop handle
    }
    return 0;
}



// error codes for 'lookforfunc'
enum {
    ERRLIB,		// 1
    ERRFUNC,	// 2
};

//
// Look for a C function named 'sym' in a dynamically loaded library
// 'path'.
// First, check whether the library is already loaded; if not, try
// to load it.
// Then, if 'sym' is '*', return true (as library has been loaded).
// Otherwise, look for symbol 'sym' in the library and push a
// C function with that symbol.
// Return 0 and 'true' or a function in the stack; in case of
// errors, return an error code and an error message in the stack.
//
static int lookforfunc(lhat_State *L, const char *path, const char *sym)
{
    void *reg = checkclib(L, path);  // check loaded C libraries
    if(reg == NULL) {  // must load library?
        reg = lsys_load(L, path, *sym == '*');  // global symbols if 'sym'=='*'
        if(reg == NULL) return ERRLIB;  // unable to load library
        addtoclib(L, path, reg);
    }
    if(*sym == '*') {  // loading only library (no function)?
        lhat_pushboolean(L, 1);  // return 'true'
        return 0;  // no errors
    }
    else {
        lhat_CFunction f = lsys_sym(L, reg, sym);
        if(f == NULL)
            return ERRFUNC;  // unable to find function
        lhat_pushcfunction(L, f);  // else create new function
        return 0;  // no errors
    }
}


static int ll_loadlib(lhat_State *L)
{
    const char *path = lhatL_checkstring(L, 1);
    const char *init = lhatL_checkstring(L, 2);
    int stat = lookforfunc(L, path, init);
    if(stat == 0)  // no errors?
        return 1;  // return the loaded function
    else {  // error; error message is on stack top
        lhat_pushnil(L);
        lhat_insert(L, -2);
        lhat_pushstring(L, (stat == ERRLIB) ? LIB_FAIL : "init");
        return 3;  // return nil, error message, and where
    }
}



//
// {======================================================
// 'require' function
// =======================================================
//


static int readable(const char *filename)
{
    FILE *f = fopen(filename, "r");  // try to open file
    if(f == NULL) return 0;  // open failed
    fclose(f);
    return 1;
}


static const char *pushnexttemplate(lhat_State *L, const char *path)
{
    while(*path == *LHAT_PATH_SEP) path++;  // skip separators
    if(*path == '\0') return NULL;  // no more templates
    const char *l = strchr(path, *LHAT_PATH_SEP);  // find next separator
    if(l == NULL) l = path + strlen(path);
    lhat_pushlstring(L, path, l - path);  // template
    return l;
}


static const char *searchpath(lhat_State *L, const char *name, const char *path, const char *sep, const char *dirsep)
{
    lhatL_Buffer msg;  // to build error message
    lhatL_buffinit(L, &msg);
    if(*sep != '\0')  // non-empty separator?
        name = lhatL_gsub(L, name, sep, dirsep);  // replace it by 'dirsep'
    while((path = pushnexttemplate(L, path)) != NULL) {
        const char *filename = lhatL_gsub(L, lhat_tostring(L, -1),
            LHAT_PATH_MARK, name);
        lhat_remove(L, -2);  // remove path template
        if(readable(filename))  // does file exist and is readable?
            return filename;  // return that file name
        lhat_pushfstring(L, "\n\tno file '%s'", filename);
        lhat_remove(L, -2);  // remove file name
        lhatL_addvalue(&msg);  // concatenate error msg. entry
    }
    lhatL_pushresult(&msg);  // create error message
    return NULL;  // not found
}


static int ll_searchpath(lhat_State *L)
{
    const char *f = searchpath(L,
        lhatL_checkstring(L, 1),
        lhatL_checkstring(L, 2),
        lhatL_optstring(L, 3, "."),
        lhatL_optstring(L, 4, LHAT_DIRSEP));
    if(f != NULL) return 1;
    else {  // error message is on top of the stack
        lhat_pushnil(L);
        lhat_insert(L, -2);
        return 2;  // return nil + error message
    }
}


static const char *findfile(lhat_State *L, const char *name, const char *pname, const char *dirsep)
{
    lhat_getfield(L, lhat_upvalueindex(1), pname);
    const char *path = lhat_tostring(L, -1);
    if(path == NULL)
        lhatL_error(L, "'package.%s' must be a string", pname);
    return searchpath(L, name, path, ".", dirsep);
}


static int checkload(lhat_State *L, int stat, const char *filename)
{
    if(stat) {  // module loaded successfully?
        lhat_pushstring(L, filename);  // will be 2nd argument to module
        return 2;  // return open function and file name
    }
    else
        return lhatL_error(L, "error loading module '%s' from file '%s':\n\t%s", lhat_tostring(L, 1), filename, lhat_tostring(L, -1));
}


static int searcher_Lhat(lhat_State *L)
{
    const char *name = lhatL_checkstring(L, 1);
    const char *filename = findfile(L, name, "path", LHAT_LSUBSEP);
    if(filename == NULL) return 1;  // module not found in this path
    return checkload(L, (lhatL_loadfile(L, filename) == LHAT_OK), filename);
}


//
// Try to find a load function for module 'modname' at file 'filename'.
// First, change '.' to '_' in 'modname'; then, if 'modname' has
// the form X-Y (that is, it has an "ignore mark"), build a function
// name "lhatopen_X" and look for it. (For compatibility, if that
// fails, it also tries "lhatopen_Y".) If there is no ignore mark,
// look for a function named "lhatopen_modname".
//
static int loadfunc(lhat_State *L, const char *filename, const char *modname)
{
    modname = lhatL_gsub(L, modname, ".", LHAT_OFSEP);
    const char *mark = strchr(modname, *LHAT_IGMARK);
    const char *openfunc;
    if(mark) {
        int stat;
        openfunc = lhat_pushlstring(L, modname, mark - modname);
        openfunc = lhat_pushfstring(L, LHAT_POF"%s", openfunc);
        stat = lookforfunc(L, filename, openfunc);
        if(stat != ERRFUNC) return stat;
        modname = mark + 1;  // else go ahead and try old-style name
    }
    openfunc = lhat_pushfstring(L, LHAT_POF"%s", modname);
    return lookforfunc(L, filename, openfunc);
}


static int searcher_C(lhat_State *L)
{
    const char *name = lhatL_checkstring(L, 1);
    const char *filename = findfile(L, name, "cpath", LHAT_CSUBSEP);
    if(filename == NULL) return 1;  // module not found in this path
    return checkload(L, (loadfunc(L, filename, name) == 0), filename);
}


static int searcher_Croot(lhat_State *L)
{
    const char *name = lhatL_checkstring(L, 1);
    const char *p = strchr(name, '.');
    if(p == NULL) return 0;  // is root
    lhat_pushlstring(L, name, p - name);
    const char *filename = findfile(L, lhat_tostring(L, -1), "cpath", LHAT_CSUBSEP);
    if(filename == NULL) return 1;  // root not found
    int stat = loadfunc(L, filename, name);
    if(stat != 0) {
        if(stat != ERRFUNC)
            return checkload(L, 0, filename);  // real error
        else {  // open function not found
            lhat_pushfstring(L, "\n\tno module '%s' in file '%s'", name, filename);
            return 1;
        }
    }
    lhat_pushstring(L, filename);  // will be 2nd argument to module
    return 2;
}


static int searcher_preload(lhat_State *L)
{
    const char *name = lhatL_checkstring(L, 1);
    lhat_getfield(L, LHAT_REGISTRYINDEX, LHAT_PRELOAD_TABLE);
    if(lhat_getfield(L, -1, name) == LHAT_TNIL)  // not found?
        lhat_pushfstring(L, "\n\tno field package.preload['%s']", name);
    return 1;
}


static void findloader(lhat_State *L, const char *name)
{
    lhatL_Buffer msg;  // to build error message
    lhatL_buffinit(L, &msg);
    // push 'package.searchers' to index 3 in the stack
    if(lhat_getfield(L, lhat_upvalueindex(1), "searchers") != LHAT_TTABLE)
        lhatL_error(L, "'package.searchers' must be a table");
    //  iterate over available searchers to find a loader
    for(int i = 1; ; i++) {
        if(lhat_rawgeti(L, 3, i) == LHAT_TNIL) {  // no more searchers?
            lhat_pop(L, 1);  // remove nil
            lhatL_pushresult(&msg);  // create error message
            lhatL_error(L, "module '%s' not found:%s", name, lhat_tostring(L, -1));
        }
        lhat_pushstring(L, name);
        lhat_call(L, 1, 2);  // call it
        if(lhat_isfunction(L, -2))  // did it find a loader?
            return;  // module loader found
        else if(lhat_isstring(L, -2)) {  // searcher returned error message?
            lhat_pop(L, 1);  // remove extra return
            lhatL_addvalue(&msg);  // concatenate error message
        }
        else
            lhat_pop(L, 2);  // remove both returns
    }
}


static int ll_require(lhat_State *L)
{
    const char *name = lhatL_checkstring(L, 1);
    lhat_settop(L, 1);  // LOADED table will be at index 2
    lhat_getfield(L, LHAT_REGISTRYINDEX, LHAT_LOADED_TABLE);
    lhat_getfield(L, 2, name);  // LOADED[name]
    if(lhat_toboolean(L, -1))  // is it there?
        return 1;  // package is already loaded
                   // else must load package
    lhat_pop(L, 1);  // remove 'getfield' result
    findloader(L, name);
    lhat_pushstring(L, name);  // pass name as argument to module loader
    lhat_insert(L, -2);  // name is 1st argument (before search data)
    lhat_call(L, 2, 1);  // run loader to load module
    if(!lhat_isnil(L, -1))  // non-nil return?
        lhat_setfield(L, 2, name);  // LOADED[name] = returned value
    if(lhat_getfield(L, 2, name) == LHAT_TNIL) {   // module set no value?
        lhat_pushboolean(L, 1);  // use true as result
        lhat_pushvalue(L, -1);  // extra copy to be returned
        lhat_setfield(L, 2, name);  // LOADED[name] = true
    }
    return 1;
}

// }======================================================



//
// {======================================================
// 'module' function
// =======================================================
//
#if defined(LHAT_COMPAT_MODULE)

//
// changes the environment variable of calling function
//
static void set_env(lhat_State *L)
{
    lhat_Debug ar;
    if(lhat_getstack(L, 1, &ar) == 0 ||
        lhat_getinfo(L, "f", &ar) == 0 ||  // get calling function
        lhat_iscfunction(L, -1))
        lhatL_error(L, "'module' not called from a L^ function");
    lhat_pushvalue(L, -2);  // copy new environment table to top
    lhat_setupvalue(L, -2, 1);
    lhat_pop(L, 1);  // remove function
}


static void dooptions(lhat_State *L, int n)
{
    int i;
    for(i = 2; i <= n; i++) {
        if(lhat_isfunction(L, i)) {  // avoid 'calling' extra info.
            lhat_pushvalue(L, i);  // get option (a function)
            lhat_pushvalue(L, -2);  // module
            lhat_call(L, 1, 0);
        }
    }
}


static void modinit(lhat_State *L, const char *modname)
{
    const char *dot;
    lhat_pushvalue(L, -1);
    lhat_setfield(L, -2, "_M");  // module._M = module
    lhat_pushstring(L, modname);
    lhat_setfield(L, -2, "_NAME");
    dot = strrchr(modname, '.');  // look for last dot in module name
    if(dot == NULL) dot = modname;
    else dot++;
    // set _PACKAGE as package name (full module name minus last part)
    lhat_pushlstring(L, modname, dot - modname);
    lhat_setfield(L, -2, "_PACKAGE");
}


static int ll_module(lhat_State *L)
{
    const char *modname = lhatL_checkstring(L, 1);
    int lastarg = lhat_gettop(L);  // last parameter
    lhatL_pushmodule(L, modname, 1);  // get/create module table
                                      // check whether table already has a _NAME field
    if(lhat_getfield(L, -1, "_NAME") != LHAT_TNIL)
        lhat_pop(L, 1);  // table is an initialized module
    else {  // no; initialize it
        lhat_pop(L, 1);
        modinit(L, modname);
    }
    lhat_pushvalue(L, -1);
    set_env(L);
    dooptions(L, lastarg);
    return 1;
}


static int ll_seeall(lhat_State *L)
{
    lhatL_checktype(L, 1, LHAT_TTABLE);
    if(!lhat_getmetatable(L, 1)) {
        lhat_createtable(L, 0, 1); // create new metatable
        lhat_pushvalue(L, -1);
        lhat_setmetatable(L, 1);
    }
    lhat_pushglobaltable(L);
    lhat_setfield(L, -2, "__index");  // mt.__index = _G
    return 0;
}

#endif
// }======================================================



static const lhatL_Reg pk_funcs[] = {
    { "loadlib", ll_loadlib },
    { "searchpath", ll_searchpath },
#if defined(LHAT_COMPAT_MODULE)
    { "seeall", ll_seeall },
#endif
    // placeholders
    { "preload", NULL },
    { "cpath", NULL },
    { "path", NULL },
    { "searchers", NULL },
    { "loaded", NULL },
    { NULL, NULL }
};


static const lhatL_Reg ll_funcs[] = {
#if defined(LHAT_COMPAT_MODULE)
    { "module", ll_module },
#endif
    { "require", ll_require },
    { NULL, NULL }
};


static void createsearcherstable(lhat_State *L)
{
    static const lhat_CFunction searchers[] = {
        searcher_preload, searcher_Lhat, searcher_C, searcher_Croot, NULL
    };
    // create 'searchers' table
    lhat_createtable(L, sizeof(searchers) / sizeof(searchers[0]) - 1, 0);
    // fill it with predefined searchers
    for(int i = 0; searchers[i] != NULL; i++) {
        lhat_pushvalue(L, -2);  // set 'package' as upvalues for all searchers
        lhat_pushcclosure(L, searchers[i], 1);
        lhat_rawseti(L, -2, i + 1);
    }
#if defined(LHAT_COMPAT_LOADERS)
    lhat_pushvalue(L, -1);  // make a copy of 'searchers' table
    lhat_setfield(L, -3, "loaders");  // put it in field 'loaders'
#endif
    lhat_setfield(L, -2, "searchers");  // put it in field 'searchers'
}


//
// create table CLIBS to keep track of loaded C libraries,
// setting a finalizer to close all libraries when closing state.
//
static void createclibstable(lhat_State *L)
{
    lhat_newtable(L);  // create CLIBS table
    lhat_createtable(L, 0, 1);  // create metatable for CLIBS
    lhat_pushcfunction(L, gctm);
    lhat_setfield(L, -2, "__gc");  // set finalizer for CLIBS table
    lhat_setmetatable(L, -2);
    lhat_rawsetp(L, LHAT_REGISTRYINDEX, &CLIBS);  // set CLIBS table in registry
}


LHATMOD_API int lhatopen_package(lhat_State *L)
{
    createclibstable(L);
    lhatL_newlib(L, pk_funcs);  // create 'package' table
    createsearcherstable(L);
    // set paths
    setpath(L, "path", LHAT_PATH_VAR, LHAT_PATH_DEFAULT);
    setpath(L, "cpath", LHAT_CPATH_VAR, LHAT_CPATH_DEFAULT);
    // store config information
    lhat_pushliteral(L, LHAT_DIRSEP "\n" LHAT_PATH_SEP "\n" LHAT_PATH_MARK "\n" LHAT_EXEC_DIR "\n" LHAT_IGMARK "\n");
    lhat_setfield(L, -2, "config");
    // set field 'loaded'
    lhatL_getsubtable(L, LHAT_REGISTRYINDEX, LHAT_LOADED_TABLE);
    lhat_setfield(L, -2, "loaded");
    // set field 'preload'
    lhatL_getsubtable(L, LHAT_REGISTRYINDEX, LHAT_PRELOAD_TABLE);
    lhat_setfield(L, -2, "preload");
    lhat_pushglobaltable(L);
    lhat_pushvalue(L, -2);  // set 'package' as upvalues for next lib
    lhatL_setfuncs(L, ll_funcs, 1);  // open lib into global table
    lhat_pop(L, 1);  // pop global table
    return 1;  // return 'package' table
}
