//
// Stack and Call structure of L^
// See Copyright Notice in lhat.h
//

#define LHAT_CORE

#include "lprefix.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"
#include "lzio.h"



#define errorstatus(s)	((s) > LHAT_YIELD)


//
// {======================================================
// Error-recovery functions
// =======================================================
//

//
// LHATI_THROW/LHATI_TRY define how L^ does exception handling. By
// default, L^ handles errors with exceptions when compiling as
// C++ code, with _longjmp/_setjmp when asked to use them, and with
// longjmp/setjmp otherwise.
//
#if !defined(LHATI_THROW)
# if defined(__cplusplus) && !defined(LHAT_USE_LONGJMP)
// C++ exceptions
#  define LHATI_THROW(L,c)	throw(c)
#  define LHATI_TRY(L,c,a) \
	try { a } catch(...) { if ((c)->status == 0) (c)->status = -1; }
#  define lhati_jmpbuf		int  // dummy variable
# elif defined(LHAT_USE_POSIX)
// in POSIX, try _longjmp/_setjmp (more efficient)
#  define LHATI_THROW(L,c)	_longjmp((c)->b, 1)
#  define LHATI_TRY(L,c,a)	if (_setjmp((c)->b) == 0) { a }
#  define lhati_jmpbuf		jmp_buf
# else
// ISO C handling with long jumps
#  define LHATI_THROW(L,c)	longjmp((c)->b, 1)
#  define LHATI_TRY(L,c,a)	if (setjmp((c)->b) == 0) { a }
#  define lhati_jmpbuf		jmp_buf
# endif
#endif



// chain list of long jump buffers
struct lhat_longjmp {
    struct lhat_longjmp *previous;
    lhati_jmpbuf b;
    volatile int status;  // error code
};


static void seterrorobj(lhat_State *L, int errcode, StkId oldtop)
{
    switch(errcode) {
    case LHAT_ERRMEM: {  // memory error?
        setsvalue2s(L, oldtop, G(L)->memerrmsg); // reuse preregistered msg.
        break;
    }
    case LHAT_ERRERR: {
        setsvalue2s(L, oldtop, lhatS_newliteral(L, "error in error handling"));
        break;
    }
    default: {
        setobjs2s(L, oldtop, L->top - 1);  // error message on current top
        break;
    }
    }
    L->top = oldtop + 1;
}


l_noret lhatD_throw(lhat_State *L, int errcode)
{
    if(L->errorJmp) {  // coroutine has an error handler?
        L->errorJmp->status = errcode;  // set status
        LHATI_THROW(L, L->errorJmp);  // jump to it
    }
    else {  // coroutine has no error handler
        global_State *g = G(L);
        L->status = cast_byte(errcode);  // mark it as dead
        if(g->maincoroutine->errorJmp) {  // main coroutine has a handler?
            setobjs2s(L, g->maincoroutine->top++, L->top - 1);  // copy error obj.
            lhatD_throw(g->maincoroutine, errcode);  // re-throw in main coroutine
        }
        else {  // no handler at all; abort
            if(g->panic) {  // panic function?
                seterrorobj(L, errcode, L->top);  // assume EXTRA_STACK
                if(L->ci->top < L->top)
                    L->ci->top = L->top;  // pushing msg. can break this invariant
                lhat_unlock(L);
                g->panic(L);  // call panic function (last chance to jump out)
            }
            abort();
        }
    }
}


int lhatD_rawrunprotected(lhat_State *L, Pfunc f, void *ud)
{
    unsigned short oldnCcalls = L->nCcalls;
    struct lhat_longjmp lj;
    lj.status = LHAT_OK;
    lj.previous = L->errorJmp;  // chain new error handler
    L->errorJmp = &lj;
    LHATI_TRY(L, &lj,
        (*f)(L, ud);
    );
    L->errorJmp = lj.previous;  // restore old error handler
    L->nCcalls = oldnCcalls;
    return lj.status;
}

// }======================================================


//
// {==================================================================
// Stack reallocation
// ===================================================================
//
static void correctstack(lhat_State *L, TValue *oldstack)
{
    L->top = (L->top - oldstack) + L->stack;
    for(Upvalue *up = L->openupval; up != NULL; up = up->u.open.next)
        up->v = (up->v - oldstack) + L->stack;
    for(CallInfo *ci = L->ci; ci != NULL; ci = ci->previous) {
        ci->top = (ci->top - oldstack) + L->stack;
        ci->func = (ci->func - oldstack) + L->stack;
        if(isLhat(ci))
            ci->u.l.base = (ci->u.l.base - oldstack) + L->stack;
    }
}


// some space for error handling
#define ERRORSTACKSIZE	(LHATI_MAXSTACK + 200)


void lhatD_reallocstack(lhat_State *L, int newsize)
{
    TValue *oldstack = L->stack;
    int lim = L->stacksize;
    lhat_assert(newsize <= LHATI_MAXSTACK || newsize == ERRORSTACKSIZE);
    lhat_assert(L->stack_last - L->stack == L->stacksize - EXTRA_STACK);
    lhatM_reallocvector(L, L->stack, L->stacksize, newsize, TValue);
    for(; lim < newsize; lim++)
        setnilvalue(L->stack + lim); // erase new segment
    L->stacksize = newsize;
    L->stack_last = L->stack + newsize - EXTRA_STACK;
    correctstack(L, oldstack);
}


void lhatD_growstack(lhat_State *L, int n)
{
    int size = L->stacksize;
    if(size > LHATI_MAXSTACK)  // error after extra size?
        lhatD_throw(L, LHAT_ERRERR);
    else {
        int needed = cast_int(L->top - L->stack) + n + EXTRA_STACK;
        int newsize = 2 * size;
        if(newsize > LHATI_MAXSTACK) newsize = LHATI_MAXSTACK;
        if(newsize < needed) newsize = needed;
        if(newsize > LHATI_MAXSTACK) {  // stack overflow?
            lhatD_reallocstack(L, ERRORSTACKSIZE);
            lhatG_runerror(L, "stack overflow");
        }
        else
            lhatD_reallocstack(L, newsize);
    }
}


static int stackinuse(lhat_State *L)
{
    CallInfo *ci;
    StkId lim = L->top;
    for(ci = L->ci; ci != NULL; ci = ci->previous) {
        if(lim < ci->top) lim = ci->top;
    }
    lhat_assert(lim <= L->stack_last);
    return cast_int(lim - L->stack) + 1;  // part of stack in use
}


void lhatD_shrinkstack(lhat_State *L)
{
    int inuse = stackinuse(L);
    int goodsize = inuse + (inuse / 8) + 2 * EXTRA_STACK;
    if(goodsize > LHATI_MAXSTACK)
        goodsize = LHATI_MAXSTACK;  // respect stack limit
    if(L->stacksize > LHATI_MAXSTACK)  // had been handling stack overflow?
        lhatE_freeCI(L);  // free all CIs (list grew because of an error)
    else
        lhatE_shrinkCI(L);  // shrink list
                            // if coroutine is currently not handling a stack overflow and its
                            // good size is smaller than current size, shrink its stack
    if(inuse <= (LHATI_MAXSTACK - EXTRA_STACK) &&
        goodsize < L->stacksize)
        lhatD_reallocstack(L, goodsize);
    else  // don't change stack
        condmovestack(L, {}, {});  // (change only for debugging)
}


void lhatD_inctop(lhat_State *L)
{
    lhatD_checkstack(L, 1);
    L->top++;
}

// }==================================================================


//
// Call a hook for the given event. Make sure there is a hook to be
// called. (Both 'L->hook' and 'L->hookmask', which triggers this
// function, can be changed asynchronously by signals.)
//
void lhatD_hook(lhat_State *L, int event, int line)
{
    lhat_Hook hook = L->hook;
    if(hook && L->allowhook) {  // make sure there is a hook
        CallInfo *ci = L->ci;
        ptrdiff_t top = savestack(L, L->top);
        ptrdiff_t ci_top = savestack(L, ci->top);
        lhat_Debug ar;
        ar.event = event;
        ar.currentline = line;
        ar.i_ci = ci;
        lhatD_checkstack(L, LHAT_MINSTACK);  // ensure minimum stack size
        ci->top = L->top + LHAT_MINSTACK;
        lhat_assert(ci->top <= L->stack_last);
        L->allowhook = 0;  // cannot call hooks inside a hook
        ci->callstatus |= CIST_HOOKED;
        lhat_unlock(L);
        (*hook)(L, &ar);
        lhat_lock(L);
        lhat_assert(!L->allowhook);
        L->allowhook = 1;
        ci->top = restorestack(L, ci_top);
        L->top = restorestack(L, top);
        ci->callstatus &= ~CIST_HOOKED;
    }
}


static void callhook(lhat_State *L, CallInfo *ci)
{
    int hook = LHAT_HOOKCALL;
    ci->u.l.savedpc++;  // hooks assume 'pc' is already incremented
    if(isLhat(ci->previous) &&
        GET_OPCODE(*(ci->previous->u.l.savedpc - 1)) == OP_TAILCALL) {
        ci->callstatus |= CIST_TAIL;
        hook = LHAT_HOOKTAILCALL;
    }
    lhatD_hook(L, hook, -1);
    ci->u.l.savedpc--;  // correct 'pc'
}


static StkId adjust_varargs(lhat_State *L, Proto *p, int actual)
{
    int nfixargs = p->numparams;
    // move fixed parameters to final position
    StkId fixed = L->top - actual;  // first fixed argument
    StkId base = L->top;  // final position of first argument
    int i;
    for(i = 0; i < nfixargs && i < actual; i++) {
        setobjs2s(L, L->top++, fixed + i);
        setnilvalue(fixed + i);  // erase original copy (for GC)
    }
    for(; i < nfixargs; i++)
        setnilvalue(L->top++);  // complete missing arguments
    return base;
}


//
// Check whether __call metafield of 'func' is a function. If so, put
// it in stack below original 'func' so that 'lhatD_precall' can call
// it. Raise an error if __call metafield is not a function.
//
static void tryfuncTM(lhat_State *L, StkId func)
{
    const TValue *tm = lhatT_gettmbyobj(L, func, TM_CALL);
    StkId p;
    if(!ttisfunction(tm))
        lhatG_typeerror(L, func, "call");
    // Open a hole inside the stack at 'func'
    for(p = L->top; p > func; p--)
        setobjs2s(L, p, p - 1);
    L->top++;  // slot ensured by caller
    setobj2s(L, func, tm);  // tag method is the new function to be called
}


//
// Given 'nres' results at 'firstResult', move 'wanted' of them to 'res'.
// Handle most typical cases (zero results for commands, one result for
// expressions, multiple results for tail calls/single parameters)
// separated.
//
static int moveresults(lhat_State *L, const TValue *firstResult, StkId res, int nres, int wanted)
{
    switch(wanted) {  // handle typical cases separately
    case 0: break;  // nothing to move
    case 1: {  // one result needed
        if(nres == 0)   // no results?
            firstResult = lhatO_nilobject;  // adjust with nil
        setobjs2s(L, res, firstResult);  // move it to proper place
        break;
    }
    case LHAT_MULTRET: {
        for(int i = 0; i < nres; i++)  // move all results to correct place
            setobjs2s(L, res + i, firstResult + i);
        L->top = res + nres;
        return 0;  // wanted == LHAT_MULTRET
    }
    default:
        if(wanted <= nres) {  // enough results?
            for(int i = 0; i < wanted; i++)  // move wanted results to correct place
                setobjs2s(L, res + i, firstResult + i);
        }
        else {  // not enough results; use all of them plus nils
            int i;
            for(i = 0; i < nres; i++)  // move all results to correct place
                setobjs2s(L, res + i, firstResult + i);
            for(; i < wanted; i++)  // complete wanted number of results
                setnilvalue(res + i);
        }
        break;
    }
    L->top = res + wanted;  // top points after the last result
    return 1;
}


//
// Finishes a function call: calls hook if necessary, removes CallInfo,
// moves current number of results to proper place; returns 0 iff call
// wanted multiple (variable number of) results.
//
int lhatD_poscall(lhat_State *L, CallInfo *ci, StkId firstResult, int nres)
{
    StkId res;
    int wanted = ci->nresults;
    if(L->hookmask & (LHAT_MASKRET | LHAT_MASKLINE)) {
        if(L->hookmask & LHAT_MASKRET) {
            ptrdiff_t fr = savestack(L, firstResult);  // hook may change stack
            lhatD_hook(L, LHAT_HOOKRET, -1);
            firstResult = restorestack(L, fr);
        }
        L->oldpc = ci->previous->u.l.savedpc;  // 'oldpc' for caller function
    }
    res = ci->func;  // res == final position of 1st result
    L->ci = ci->previous;  // back to caller
                           // move results to proper place
    return moveresults(L, firstResult, res, nres, wanted);
}



#define next_ci(L) (L->ci = (L->ci->next ? L->ci->next : lhatE_extendCI(L)))


// macro to check stack size, preserving 'p'
#define checkstackp(L,n,p)  \
  lhatD_checkstackaux(L, n, \
    ptrdiff_t t__ = savestack(L, p);  /* save 'p' */\
    lhatC_checkGC(L),  /* stack grow uses memory */\
    p = restorestack(L, t__))  // 'pos' part: restore 'p'


//
// Prepares a function call: checks the stack, creates a new CallInfo
// entry, fills in the relevant information, calls hook if needed.
// If function is a C function, does the call, too. (Otherwise, leave
// the execution ('lhatV_execute') to the caller, to allow stackless
// calls.) Returns true iff function has been executed (C function).
//
int lhatD_precall(lhat_State *L, StkId func, int nresults)
{
    lhat_CFunction f;
    CallInfo *ci;
    switch(ttype(func)) {
    case LHAT_TCCL:  // C closure
        f = clCvalue(func)->f;
        goto Cfunc;
    case LHAT_TLCF:  // light C function
        f = fvalue(func);
    Cfunc: {
        int n;  // number of returns
        checkstackp(L, LHAT_MINSTACK, func);  // ensure minimum stack size
        ci = next_ci(L);  // now 'enter' new function
        ci->nresults = nresults;
        ci->func = func;
        ci->top = L->top + LHAT_MINSTACK;
        lhat_assert(ci->top <= L->stack_last);
        ci->callstatus = 0;
        if(L->hookmask & LHAT_MASKCALL)
            lhatD_hook(L, LHAT_HOOKCALL, -1);
        lhat_unlock(L);
        n = (*f)(L);  // do the actual call
        lhat_lock(L);
        api_checknelems(L, n);
        lhatD_poscall(L, ci, L->top - n, n);
        return 1;
        }
    case LHAT_TLCL: {  // L^ function: prepare its call
        StkId base;
        Proto *p = clLvalue(func)->p;
        int n = cast_int(L->top - func) - 1;  // number of real arguments
        int fsize = p->maxstacksize;  // frame size
        checkstackp(L, fsize, func);
        if(p->is_vararg)
            base = adjust_varargs(L, p, n);
        else {  // non vararg function
            for(; n < p->numparams; n++)
                setnilvalue(L->top++);  // complete missing arguments
            base = func + 1;
        }
        ci = next_ci(L);  // now 'enter' new function
        ci->nresults = nresults;
        ci->func = func;
        ci->u.l.base = base;
        L->top = ci->top = base + fsize;
        lhat_assert(ci->top <= L->stack_last);
        ci->u.l.savedpc = p->code;  // starting point
        ci->callstatus = CIST_LHAT;
        if(L->hookmask & LHAT_MASKCALL)
            callhook(L, ci);
        return 0;
    }
    default: // not a function
        checkstackp(L, 1, func);  // ensure space for metamethod
        tryfuncTM(L, func);  // try to get '__call' metamethod
        return lhatD_precall(L, func, nresults);  // now it must be a function
    }
}


//
// Check appropriate error for stack overflow ("regular" overflow or
// overflow while handling stack overflow). If 'nCalls' is larger than
// LHATI_MAXCCALLS (which means it is handling a "regular" overflow) but
// smaller than 9/8 of LHATI_MAXCCALLS, does not report an error (to
// allow overflow handling to work)
//
static void stackerror(lhat_State *L)
{
    if(L->nCcalls == LHATI_MAXCCALLS)
        lhatG_runerror(L, "C stack overflow");
    else if(L->nCcalls >= (LHATI_MAXCCALLS + (LHATI_MAXCCALLS >> 3)))
        lhatD_throw(L, LHAT_ERRERR);  // error while handing stack error
}


//
// Call a function (C or L^). The function to be called is at *func.
// The arguments are on the stack, right after the function.
// When returns, all the results are on the stack, starting at the original
// function position.
//
void lhatD_call(lhat_State *L, StkId func, int nResults)
{
    if(++L->nCcalls >= LHATI_MAXCCALLS)
        stackerror(L);
    if(!lhatD_precall(L, func, nResults))  // is a L^ function?
        lhatV_execute(L);  // call it
    L->nCcalls--;
}


//
// Similar to 'lhatD_call', but does not allow yields during the call
//
void lhatD_callnoyield(lhat_State *L, StkId func, int nResults)
{
    L->nny++;
    lhatD_call(L, func, nResults);
    L->nny--;
}


//
// Completes the execution of an interrupted C function, calling its
// continuation function.
//
static void finishCcall(lhat_State *L, int status)
{
    CallInfo *ci = L->ci;
    int n;
    // must have a continuation and must be able to call it
    lhat_assert(ci->u.c.k != NULL && L->nny == 0);
    // error status can only happen in a protected call
    lhat_assert((ci->callstatus & CIST_YPCALL) || status == LHAT_YIELD);
    if(ci->callstatus & CIST_YPCALL) {  // was inside a pcall?
        ci->callstatus &= ~CIST_YPCALL;  // continuation is also inside it
        L->errfunc = ci->u.c.old_errfunc;  // with the same error function
    }
    // finish 'lhat_callk'/'lhat_pcall'; CIST_YPCALL and 'errfunc' already handled
    adjustresults(L, ci->nresults);
    lhat_unlock(L);
    n = (*ci->u.c.k)(L, status, ci->u.c.ctx);  // call continuation function
    lhat_lock(L);
    api_checknelems(L, n);
    lhatD_poscall(L, ci, L->top - n, n);  // finish 'lhatD_precall'
}


//
// Executes "full continuation" (everything in the stack) of a
// previously interrupted coroutine until the stack is empty (or another
// interruption long-jumps out of the loop). If the coroutine is
// recovering from an error, 'ud' points to the error status, which must
// be passed to the first continuation function (otherwise the default
// status is LHAT_YIELD).
//
static void unroll(lhat_State *L, void *ud)
{
    if(ud != NULL)  // error status?
        finishCcall(L, *(int *)ud);  // finish 'lhat_pcallk' callee
    while(L->ci != &L->base_ci) {  // something in the stack
        if(!isLhat(L->ci))  // C function?
            finishCcall(L, LHAT_YIELD);  // complete its execution
        else {  // L^ function
            lhatV_finishOp(L);  // finish interrupted instruction
            lhatV_execute(L);  // execute down to higher C 'boundary'
        }
    }
}


//
// Try to find a suspended protected call (a "recover point") for the
// given coroutine.
//
static CallInfo *findpcall(lhat_State *L)
{
    CallInfo *ci;
    for(ci = L->ci; ci != NULL; ci = ci->previous) {  // search for a pcall
        if(ci->callstatus & CIST_YPCALL)
            return ci;
    }
    return NULL;  // no pending pcall
}


//
// Recovers from an error in a coroutine. Finds a recover point (if
// there is one) and completes the execution of the interrupted
// 'lhatD_pcall'. If there is no recover point, returns zero.
//
static int recover(lhat_State *L, int status)
{
    StkId oldtop;
    CallInfo *ci = findpcall(L);
    if(ci == NULL) return 0;  // no recovery point
                              // "finish" lhatD_pcall
    oldtop = restorestack(L, ci->extra);
    lhatF_close(L, oldtop);
    seterrorobj(L, status, oldtop);
    L->ci = ci;
    L->allowhook = getoah(ci->callstatus);  // restore original 'allowhook'
    L->nny = 0;  // should be zero to be yieldable
    lhatD_shrinkstack(L);
    L->errfunc = ci->u.c.old_errfunc;
    return 1;  // continue running the coroutine
}


//
// Signal an error in the call to 'lhat_resume', not in the execution
// of the coroutine itself. (Such errors should not be handled by any
// coroutine error handler and should not kill the coroutine.)
//
static int resume_error(lhat_State *L, const char *msg, int narg)
{
    L->top -= narg;  // remove args from the stack
    setsvalue2s(L, L->top, lhatS_new(L, msg));  // push error message
    api_incr_top(L);
    lhat_unlock(L);
    return LHAT_ERRRUN;
}


//
// Do the work for 'lhat_resume' in protected mode. Most of the work
// depends on the status of the coroutine: initial state, suspended
// inside a hook, or regularly suspended (optionally with a continuation
// function), plus erroneous cases: non-suspended coroutine or dead
// coroutine.
//
static void resume(lhat_State *L, void *ud)
{
    int n = *(cast(int*, ud));  // number of arguments
    StkId firstArg = L->top - n;  // first argument
    CallInfo *ci = L->ci;
    if(L->status == LHAT_OK) {  // starting a coroutine?
        if(!lhatD_precall(L, firstArg - 1, LHAT_MULTRET))  // L^ function?
            lhatV_execute(L);  // call it
    }
    else {  // resuming from previous yield
        lhat_assert(L->status == LHAT_YIELD);
        L->status = LHAT_OK;  // mark that it is running (again)
        ci->func = restorestack(L, ci->extra);
        if(isLhat(ci))  // yielded inside a hook?
            lhatV_execute(L);  // just continue running L^ code
        else {  // 'common' yield
            if(ci->u.c.k != NULL) {  // does it have a continuation function?
                lhat_unlock(L);
                n = (*ci->u.c.k)(L, LHAT_YIELD, ci->u.c.ctx); // call continuation
                lhat_lock(L);
                api_checknelems(L, n);
                firstArg = L->top - n;  // yield results come from continuation
            }
            lhatD_poscall(L, ci, firstArg, n);  // finish 'lhatD_precall'
        }
        unroll(L, NULL);  // run continuation
    }
}


LHAT_API int lhat_resume(lhat_State *L, lhat_State *from, int nargs)
{
    int status;
    unsigned short oldnny = L->nny;  // save "number of non-yieldable" calls
    lhat_lock(L);
    if(L->status == LHAT_OK) {  // may be starting a coroutine
        if(L->ci != &L->base_ci)  // not in base level?
            return resume_error(L, "cannot resume non-suspended coroutine", nargs);
    }
    else if(L->status != LHAT_YIELD)
        return resume_error(L, "cannot resume dead coroutine", nargs);
    L->nCcalls = (from) ? from->nCcalls + 1 : 1;
    if(L->nCcalls >= LHATI_MAXCCALLS)
        return resume_error(L, "C stack overflow", nargs);
    lhati_userstateresume(L, nargs);
    L->nny = 0;  // allow yields
    api_checknelems(L, (L->status == LHAT_OK) ? nargs + 1 : nargs);
    status = lhatD_rawrunprotected(L, resume, &nargs);
    if(status == -1)  // error calling 'lhat_resume'?
        status = LHAT_ERRRUN;
    else {  // continue running after recoverable errors
        while(errorstatus(status) && recover(L, status)) {
            // unroll continuation
            status = lhatD_rawrunprotected(L, unroll, &status);
        }
        if(errorstatus(status)) {  // unrecoverable error?
            L->status = cast_byte(status);  // mark coroutine as 'dead'
            seterrorobj(L, status, L->top);  // push error message
            L->ci->top = L->top;
        }
        else lhat_assert(status == L->status);  // normal end or yield
    }
    L->nny = oldnny;  // restore 'nny'
    L->nCcalls--;
    lhat_assert(L->nCcalls == ((from) ? from->nCcalls : 0));
    lhat_unlock(L);
    return status;
}


LHAT_API int lhat_isyieldable(lhat_State *L)
{
    return (L->nny == 0);
}


LHAT_API int lhat_yieldk(lhat_State *L, int nresults, lhat_KContext ctx, lhat_KFunction k)
{
    CallInfo *ci = L->ci;
    lhati_userstateyield(L, nresults);
    lhat_lock(L);
    api_checknelems(L, nresults);
    if(L->nny > 0) {
        if(L != G(L)->maincoroutine)
            lhatG_runerror(L, "attempt to yield across a C-call boundary");
        else
            lhatG_runerror(L, "attempt to yield from outside a coroutine");
    }
    L->status = LHAT_YIELD;
    ci->extra = savestack(L, ci->func);  // save current 'func'
    if(isLhat(ci)) {  // inside a hook?
        api_check(L, k == NULL, "hooks cannot continue after yielding");
    }
    else {
        if((ci->u.c.k = k) != NULL)  // is there a continuation?
            ci->u.c.ctx = ctx;  // save context
        ci->func = L->top - nresults - 1;  // protect stack below results
        lhatD_throw(L, LHAT_YIELD);
    }
    lhat_assert(ci->callstatus & CIST_HOOKED);  // must be inside a hook
    lhat_unlock(L);
    return 0;  // return to 'lhatD_hook'
}


int lhatD_pcall(lhat_State *L, Pfunc func, void *u, ptrdiff_t old_top, ptrdiff_t ef)
{
    int status;
    CallInfo *old_ci = L->ci;
    lu_byte old_allowhooks = L->allowhook;
    unsigned short old_nny = L->nny;
    ptrdiff_t old_errfunc = L->errfunc;
    L->errfunc = ef;
    status = lhatD_rawrunprotected(L, func, u);
    if(status != LHAT_OK) {  // an error occurred?
        StkId oldtop = restorestack(L, old_top);
        lhatF_close(L, oldtop);  // close possible pending closures
        seterrorobj(L, status, oldtop);
        L->ci = old_ci;
        L->allowhook = old_allowhooks;
        L->nny = old_nny;
        lhatD_shrinkstack(L);
    }
    L->errfunc = old_errfunc;
    return status;
}



//
// Execute a protected parser.
//
struct SParser {  // data to 'f_parser'
    ZIO *z;
    Mbuffer buff;  // dynamic structure used by the scanner
    Dyndata dyd;  // dynamic structures used by the parser
    const char *mode;
    const char *name;
};


static void checkmode(lhat_State *L, const char *mode, const char *x)
{
    if(mode && strchr(mode, x[0]) == NULL) {
        lhatO_pushfstring(L,
            "attempt to load a %s chunk (mode is '%s')", x, mode);
        lhatD_throw(L, LHAT_ERRSYNTAX);
    }
}


static void f_parser(lhat_State *L, void *ud)
{
    LClosure *cl;
    struct SParser *p = cast(struct SParser *, ud);
    int c = zgetc(p->z);  // read first character
    if(c == LHAT_SIGNATURE[0]) {
        checkmode(L, p->mode, "binary");
        cl = lhatU_undump(L, p->z, p->name);
    }
    else {
        checkmode(L, p->mode, "text");
        cl = lhatY_parser(L, p->z, &p->buff, &p->dyd, p->name, c);
    }
    lhat_assert(cl->nupvalues == cl->p->sizeupvalues);
    lhatF_initupvals(L, cl);
}


int lhatD_protectedparser(lhat_State *L, ZIO *z, const char *name, const char *mode)
{
    struct SParser p;
    int status;
    L->nny++;  // cannot yield during parsing
    p.z = z; p.name = name; p.mode = mode;
    p.dyd.actvar.arr = NULL; p.dyd.actvar.size = 0;
    p.dyd.gt.arr = NULL; p.dyd.gt.size = 0;
    p.dyd.label.arr = NULL; p.dyd.label.size = 0;
    lhatZ_initbuffer(L, &p.buff);
    status = lhatD_pcall(L, f_parser, &p, savestack(L, L->top), L->errfunc);
    lhatZ_freebuffer(L, &p.buff);
    lhatM_freearray(L, p.dyd.actvar.arr, p.dyd.actvar.size);
    lhatM_freearray(L, p.dyd.gt.arr, p.dyd.gt.size);
    lhatM_freearray(L, p.dyd.label.arr, p.dyd.label.size);
    L->nny--;
    return status;
}
