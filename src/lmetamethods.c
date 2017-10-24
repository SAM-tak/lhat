//
// meta methods
// See Copyright Notice in lhat.h
//

#define LHAT_CORE

#include "lprefix.h"

#include <string.h>

#include "lhat.h"

#include "ldebug.h"
#include "ldo.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lmetamethods.h"
#include "lvm.h"


static const char udatatypename[] = "userdata";

LHATI_DDEF const char *const lhatT_typenames_[LHAT_TOTALTAGS] = {
    "no value",
    "nil", "boolean", udatatypename, "number",
    "string", "table", "function", udatatypename, "coroutine",
    "proto" // this last case is used for tests only
};


void lhatT_init(lhat_State *L)
{
    static const char *const lhatT_eventname[] = {  // ORDER MM
        "__index", "__newindex",
        "__gc", "__mode", "__len", "__eq",
        "__add", "__sub", "__mul", "__mod", "__pow",
        "__div", "__idiv",
        "__band", "__bor", "__bxor", "__shl", "__shr",
        "__unm", "__bnot", "__lt", "__le",
        "__concat", "__call"
    };
    for(int i = 0; i<MM_N; i++) {
        G(L)->tmname[i] = lhatS_new(L, lhatT_eventname[i]);
        lhatC_fix(L, obj2gco(G(L)->tmname[i]));  // never collect these names
    }
}


//
// function to be used with macro "fastmm": optimized for absence of
// tag methods
//
const TValue *lhatT_getMM(Table *events, MetaMethod event, TString *ename)
{
    const TValue *tm = lhatH_getshortstr(events, ename);
    lhat_assert(event <= MM_EQ);
    if(ttisnil(tm)) {  // no tag method?
        events->flags |= cast_byte(1u << event);  // cache this fact
        return NULL;
    }
    else return tm;
}


const TValue *lhatT_getMMByObj(lhat_State *L, const TValue *o, MetaMethod event)
{
    Table *mt;
    switch(ttnov(o)) {
    case LHAT_TTABLE:
        mt = hvalue(o)->metatable;
        break;
    case LHAT_TUSERDATA:
        mt = uvalue(o)->metatable;
        break;
    default:
        mt = G(L)->mt[ttnov(o)];
    }
    return (mt ? lhatH_getshortstr(mt, G(L)->tmname[event]) : lhatO_nilobject);
}


//
// Return the name of the type of an object. For tables and userdata
// with metatable, use their '__name' metafield, if present.
//
const char *lhatT_objtypename(lhat_State *L, const TValue *o)
{
    Table *mt;
    if((ttistable(o) && (mt = hvalue(o)->metatable) != NULL) ||
        (ttisfulluserdata(o) && (mt = uvalue(o)->metatable) != NULL)) {
        const TValue *name = lhatH_getshortstr(mt, lhatS_new(L, "__name"));
        if(ttisstring(name))  // is '__name' a string?
            return getstr(tsvalue(name));  // use it as type name
    }
    return ttypename(ttnov(o));  // else use standard type name
}


void lhatT_callMM(lhat_State *L, const TValue *f, const TValue *p1, const TValue *p2, TValue *p3, int hasres)
{
    ptrdiff_t result = savestack(L, p3);
    StkId func = L->top;
    setobj2s(L, func, f);  // push function (assume EXTRA_STACK)
    setobj2s(L, func + 1, p1);  // 1st argument
    setobj2s(L, func + 2, p2);  // 2nd argument
    L->top += 3;
    if(!hasres)  // no result? 'p3' is third argument
        setobj2s(L, L->top++, p3);  // 3rd argument
                                    // metamethod may yield only when called from L^ code
    if(isLhat(L->ci))
        lhatD_call(L, func, hasres);
    else
        lhatD_callnoyield(L, func, hasres);
    if(hasres) {  // if has result, move it to its place
        p3 = restorestack(L, result);
        setobjs2s(L, p3, --L->top);
    }
}


int lhatT_callBinMM(lhat_State *L, const TValue *p1, const TValue *p2, StkId res, MetaMethod event)
{
    const TValue *tm = lhatT_getMMByObj(L, p1, event);  // try first operand
    if(ttisnil(tm))
        tm = lhatT_getMMByObj(L, p2, event);  // try second operand
    if(ttisnil(tm)) return 0;
    lhatT_callMM(L, tm, p1, p2, res, 1);
    return 1;
}


void lhatT_tryBinMM(lhat_State *L, const TValue *p1, const TValue *p2, StkId res, MetaMethod event)
{
    if(!lhatT_callBinMM(L, p1, p2, res, event)) {
        switch(event) {
        case MM_CONCAT:
            lhatG_concaterror(L, p1, p2);
            // call never returns, but to avoid warnings:// FALLTHROUGH
        case MM_BAND: case MM_BOR: case MM_BXOR:
        case MM_SHL: case MM_SHR: case MM_BNOT: {
            lhat_Number dummy;
            if(tonumber(p1, &dummy) && tonumber(p2, &dummy))
                lhatG_tointerror(L, p1, p2);
            else
                lhatG_opinterror(L, p1, p2, "perform bitwise operation on");
        }
                     // calls never return, but to avoid warnings:// FALLTHROUGH
        default:
            lhatG_opinterror(L, p1, p2, "perform arithmetic on");
        }
    }
}


int lhatT_callOrderMM(lhat_State *L, const TValue *p1, const TValue *p2, MetaMethod event)
{
    if(!lhatT_callBinMM(L, p1, p2, L->top, event))
        return -1;  // no metamethod
    else
        return !l_isfalse(L->top);
}
