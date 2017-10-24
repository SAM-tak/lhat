//
// Standard Operating System library
// See Copyright Notice in lhat.h
//

#define LHAT_LIB

#include "lprefix.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"


//
// {==================================================================
// List of valid conversion specifiers for the 'strftime' function;
// options are grouped by length; group of length 2 start with '||'.
// ===================================================================
//
#if !defined(LHAT_STRFTIMEOPTIONS)
// options for ANSI C 89 (only 1-char options)
# define L_STRFTIMEC89		"aAbBcdHIjmMpSUwWxXyYZ%"
// options for ISO C 99 and POSIX
# define L_STRFTIMEC99 "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%" \
    "||" "EcECExEXEyEY" "OdOeOHOIOmOMOSOuOUOVOwOWOy"  // two-char options
// options for Windows
# define L_STRFTIMEWIN "aAbBcdHIjmMpSUwWxXyYzZ%" \
    "||" "#c#x#d#H#I#j#m#M#S#U#w#W#y#Y"  // two-char options
# if defined(LHAT_USE_WINDOWS)
#  define LHAT_STRFTIMEOPTIONS	L_STRFTIMEWIN
# elif defined(LHAT_USE_C89)
#  define LHAT_STRFTIMEOPTIONS	L_STRFTIMEC89
# else  // C99 specification
#  define LHAT_STRFTIMEOPTIONS	L_STRFTIMEC99
# endif
#endif
// }==================================================================


//
// {==================================================================
// Configuration for time-related stuff
// ===================================================================
//

#if !defined(l_time_t)
//
// type to represent time_t in L^
//
#define l_timet			lhat_Integer
#define l_pushtime(L,t)		lhat_pushinteger(L,(lhat_Integer)(t))

static time_t l_checktime(lhat_State *L, int arg)
{
    lhat_Integer t = lhatL_checkinteger(L, arg);
    lhatL_argcheck(L, (time_t)t == t, arg, "time out-of-bounds");
    return (time_t)t;
}
#endif


#if !defined(l_gmtime)
//
// By default, L^ uses gmtime/localtime, except when POSIX is available,
// where it uses gmtime_r/localtime_r
//
# if defined(LHAT_USE_POSIX)
#  define l_gmtime(t,r)		gmtime_r(t,r)
#  define l_localtime(t,r)	localtime_r(t,r)
# else
// ISO C definitions
#  define l_gmtime(t,r)		((void)(r)->tm_sec, gmtime(t))
#  define l_localtime(t,r)  ((void)(r)->tm_sec, localtime(t))
# endif
#endif

// }==================================================================


//
// {==================================================================
// Configuration for 'tmpnam':
// By default, L^ uses tmpnam except when POSIX is available, where
// it uses mkstemp.
// ===================================================================
//
#if !defined(lhat_tmpnam)
# if defined(LHAT_USE_POSIX)
#  include <unistd.h>
#  define LHAT_TMPNAMBUFSIZE	32
#  if !defined(LHAT_TMPNAMTEMPLATE)
#   define LHAT_TMPNAMTEMPLATE	"/tmp/lhat_XXXXXX"
#  endif
#  define lhat_tmpnam(b,e) { \
        strcpy(b, LHAT_TMPNAMTEMPLATE); \
        e = mkstemp(b); \
        if (e != -1) close(e); \
        e = (e == -1); }
# else
// ISO C definitions
#  define LHAT_TMPNAMBUFSIZE	L_tmpnam
#  define lhat_tmpnam(b,e)		{ e = (tmpnam(b) == NULL); }
# endif
#endif
// }==================================================================




static int os_execute(lhat_State *L)
{
    const char *cmd = lhatL_optstring(L, 1, NULL);
    int stat = system(cmd);
    if(cmd != NULL)
        return lhatL_execresult(L, stat);
    else {
        lhat_pushboolean(L, stat);  // true if there is a shell
        return 1;
    }
}


static int os_remove(lhat_State *L)
{
    const char *filename = lhatL_checkstring(L, 1);
    return lhatL_fileresult(L, remove(filename) == 0, filename);
}


static int os_rename(lhat_State *L)
{
    const char *fromname = lhatL_checkstring(L, 1);
    const char *toname = lhatL_checkstring(L, 2);
    return lhatL_fileresult(L, rename(fromname, toname) == 0, NULL);
}


static int os_tmpname(lhat_State *L)
{
    char buff[LHAT_TMPNAMBUFSIZE];
    int err;
    lhat_tmpnam(buff, err);
    if(err)
        return lhatL_error(L, "unable to generate a unique filename");
    lhat_pushstring(L, buff);
    return 1;
}


static int os_getenv(lhat_State *L)
{
    lhat_pushstring(L, getenv(lhatL_checkstring(L, 1)));  // if NULL push nil
    return 1;
}


static int os_clock(lhat_State *L)
{
    lhat_pushnumber(L, ((lhat_Number)clock()) / (lhat_Number)CLOCKS_PER_SEC);
    return 1;
}


//
// {======================================================
// Time/Date operations
// { year=%Y, month=%m, day=%d, hour=%H, min=%M, sec=%S,
//   wday=%w+1, yday=%j, isdst=? }
// =======================================================
//

static void setfield(lhat_State *L, const char *key, int value)
{
    lhat_pushinteger(L, value);
    lhat_setfield(L, -2, key);
}

static void setboolfield(lhat_State *L, const char *key, int value)
{
    if(value < 0)  // undefined?
        return;  // does not set field
    lhat_pushboolean(L, value);
    lhat_setfield(L, -2, key);
}


//
// Set all fields from structure 'tm' in the table on top of the stack
//
static void setallfields(lhat_State *L, struct tm *stm)
{
    setfield(L, "sec", stm->tm_sec);
    setfield(L, "min", stm->tm_min);
    setfield(L, "hour", stm->tm_hour);
    setfield(L, "day", stm->tm_mday);
    setfield(L, "month", stm->tm_mon + 1);
    setfield(L, "year", stm->tm_year + 1900);
    setfield(L, "wday", stm->tm_wday + 1);
    setfield(L, "yday", stm->tm_yday + 1);
    setboolfield(L, "isdst", stm->tm_isdst);
}


static int getboolfield(lhat_State *L, const char *key)
{
    int res = (lhat_getfield(L, -1, key) == LHAT_TNIL) ? -1 : lhat_toboolean(L, -1);
    lhat_pop(L, 1);
    return res;
}


// maximum value for date fields (to avoid arithmetic overflows with 'int')
#if !defined(L_MAXDATEFIELD)
# define L_MAXDATEFIELD	(INT_MAX / 2)
#endif

static int getfield(lhat_State *L, const char *key, int d, int delta)
{
    int t = lhat_getfield(L, -1, key);  // get field and its type
    int isnum;
    lhat_Integer res = lhat_tointegerx(L, -1, &isnum);
    if(!isnum) {  // field is not an integer?
        if(t != LHAT_TNIL)  // some other value?
            return lhatL_error(L, "field '%s' is not an integer", key);
        else if(d < 0)  // absent field; no default?
            return lhatL_error(L, "field '%s' missing in date table", key);
        res = d;
    }
    else {
        if(!(-L_MAXDATEFIELD <= res && res <= L_MAXDATEFIELD))
            return lhatL_error(L, "field '%s' is out-of-bound", key);
        res -= delta;
    }
    lhat_pop(L, 1);
    return (int)res;
}


static const char *checkoption(lhat_State *L, const char *conv, ptrdiff_t convlen, char *buff)
{
    const char *option = LHAT_STRFTIMEOPTIONS;
    int oplen = 1;  // length of options being checked
    for(; *option != '\0' && oplen <= convlen; option += oplen) {
        if(*option == '|')  // next block?
            oplen++;  // will check options with next length (+1)
        else if(memcmp(conv, option, oplen) == 0) {  // match?
            memcpy(buff, conv, oplen);  // copy valid option to buffer
            buff[oplen] = '\0';
            return conv + oplen;  // return next item
        }
    }
    lhatL_argerror(L, 1, lhat_pushfstring(L, "invalid conversion specifier '%%%s'", conv));
    return conv;  // to avoid warnings
}


// maximum size for an individual 'strftime' item
#define SIZETIMEFMT	250


static int os_date(lhat_State *L)
{
    size_t slen;
    const char *s = lhatL_optlstring(L, 1, "%c", &slen);
    time_t t = lhatL_opt(L, l_checktime, 2, time(NULL));
    const char *se = s + slen;  // 's' end
    struct tm tmr, *stm;
    if(*s == '!') {  // UTC?
        stm = l_gmtime(&t, &tmr);
        s++;  // skip '!'
    }
    else
        stm = l_localtime(&t, &tmr);
    if(stm == NULL)  // invalid date?
        lhatL_error(L, "time result cannot be represented in this installation");
    if(strcmp(s, "*t") == 0) {
        lhat_createtable(L, 0, 9);  // 9 = number of fields
        setallfields(L, stm);
    }
    else {
        char cc[4];  // buffer for individual conversion specifiers
        lhatL_Buffer b;
        cc[0] = '%';
        lhatL_buffinit(L, &b);
        while(s < se) {
            if(*s != '%')  // not a conversion specifier?
                lhatL_addchar(&b, *s++);
            else {
                size_t reslen;
                char *buff = lhatL_prepbuffsize(&b, SIZETIMEFMT);
                s++;  // skip '%'
                s = checkoption(L, s, se - s, cc + 1);  // copy specifier to 'cc'
                reslen = strftime(buff, SIZETIMEFMT, cc, stm);
                lhatL_addsize(&b, reslen);
            }
        }
        lhatL_pushresult(&b);
    }
    return 1;
}


static int os_time(lhat_State *L)
{
    time_t t;
    if(lhat_isnoneornil(L, 1))  // called without args?
        t = time(NULL);  // get current time
    else {
        struct tm ts;
        lhatL_checktype(L, 1, LHAT_TTABLE);
        lhat_settop(L, 1);  // make sure table is at the top
        ts.tm_sec = getfield(L, "sec", 0, 0);
        ts.tm_min = getfield(L, "min", 0, 0);
        ts.tm_hour = getfield(L, "hour", 12, 0);
        ts.tm_mday = getfield(L, "day", -1, 0);
        ts.tm_mon = getfield(L, "month", -1, 1);
        ts.tm_year = getfield(L, "year", -1, 1900);
        ts.tm_isdst = getboolfield(L, "isdst");
        t = mktime(&ts);
        setallfields(L, &ts);  // update fields with normalized values
    }
    if(t != (time_t)(l_timet)t || t == (time_t)(-1))
        lhatL_error(L, "time result cannot be represented in this installation");
    l_pushtime(L, t);
    return 1;
}


static int os_difftime(lhat_State *L)
{
    time_t t1 = l_checktime(L, 1);
    time_t t2 = l_checktime(L, 2);
    lhat_pushnumber(L, (lhat_Number)difftime(t1, t2));
    return 1;
}

// }======================================================


static int os_setlocale(lhat_State *L)
{
    static const int cat[] = { LC_ALL, LC_COLLATE, LC_CTYPE, LC_MONETARY, LC_NUMERIC, LC_TIME };
    static const char *const catnames[] = { "all", "collate", "ctype", "monetary", "numeric", "time", NULL };
    const char *l = lhatL_optstring(L, 1, NULL);
    int op = lhatL_checkoption(L, 2, "all", catnames);
    lhat_pushstring(L, setlocale(cat[op], l));
    return 1;
}


static int os_exit(lhat_State *L)
{
    int status;
    if(lhat_isboolean(L, 1))
        status = (lhat_toboolean(L, 1) ? EXIT_SUCCESS : EXIT_FAILURE);
    else
        status = (int)lhatL_optinteger(L, 1, EXIT_SUCCESS);
    if(lhat_toboolean(L, 2))
        lhat_delete(L);
    if(L) exit(status);  // 'if' to avoid warnings for unreachable 'return'
    return 0;
}


static const lhatL_Reg syslib[] = {
    { "clock",     os_clock },
    { "date",      os_date },
    { "difftime",  os_difftime },
    { "execute",   os_execute },
    { "exit",      os_exit },
    { "getenv",    os_getenv },
    { "remove",    os_remove },
    { "rename",    os_rename },
    { "setlocale", os_setlocale },
    { "time",      os_time },
    { "tmpname",   os_tmpname },
    { NULL, NULL }
};
// }======================================================

LHATMOD_API int lhatopen_os(lhat_State *L)
{
    lhatL_newlib(L, syslib);
    return 1;
}
