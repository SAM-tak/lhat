//
// Global State
// See Copyright Notice in lhat.h
//

#define LHAT_CORE

#include "lprefix.h"

#include <stddef.h>
#include <string.h>

#include "lhat.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lmetamethods.h"


#if !defined(LHATI_GCPAUSE)
# define LHATI_GCPAUSE	200  // 200%
#endif

#if !defined(LHATI_GCMUL)
# define LHATI_GCMUL	200 // GC runs 'twice the speed' of memory allocation
#endif


//
// a macro to help the creation of a unique random seed when a state is
// created; the seed is used to randomize hashes.
//
#if !defined(lhati_makeseed)
# include <time.h>
# define lhati_makeseed()		cast(unsigned int, time(NULL))
#endif



//
// coroutine state + extra space
//
typedef struct LX {
    lu_byte extra_[LHAT_EXTRASPACE];
    lhat_State l;
} LX;


//
// Main coroutine combines a coroutine state and the global state
//
typedef struct LG {
    LX l;
    global_State g;
} LG;



#define fromstate(L)	(cast(LX *, cast(lu_byte *, (L)) - offsetof(LX, l)))


//
// Compute an initial seed as random as possible. Rely on Address Space
// Layout Randomization (if present) to increase randomness..
//
#define addbuff(b,p,e) \
  { size_t t = cast(size_t, e); \
    memcpy(b + p, &t, sizeof(t)); p += sizeof(t); }

static unsigned int makeseed(lhat_State *L)
{
    char buff[4 * sizeof(size_t)];
    unsigned int h = lhati_makeseed();
    int p = 0;
    addbuff(buff, p, L);  // heap variable
    addbuff(buff, p, &h);  // local variable
    addbuff(buff, p, lhatO_nilobject);  // global variable
    addbuff(buff, p, &lhat_newstate);  // public function
    lhat_assert(p == sizeof(buff));
    return lhatS_hash(buff, p, h);
}


//
// set GCdebt to a new value keeping the value (totalbytes + GCdebt)
// invariant (and avoiding underflows in 'totalbytes')
//
void lhatE_setdebt(global_State *g, l_mem debt)
{
    l_mem tb = gettotalbytes(g);
    lhat_assert(tb > 0);
    if(debt < tb - MAX_LMEM)
        debt = tb - MAX_LMEM;  // will make 'totalbytes == MAX_LMEM'
    g->totalbytes = tb - debt;
    g->GCdebt = debt;
}


CallInfo *lhatE_extendCI(lhat_State *L)
{
    CallInfo *ci = lhatM_new(L, CallInfo);
    lhat_assert(L->ci->next == NULL);
    L->ci->next = ci;
    ci->previous = L->ci;
    ci->next = NULL;
    L->nci++;
    return ci;
}


//
// free all CallInfo structures not in use by a coroutine
//
void lhatE_freeCI(lhat_State *L)
{
    CallInfo *ci = L->ci;
    CallInfo *next = ci->next;
    ci->next = NULL;
    while((ci = next) != NULL) {
        next = ci->next;
        lhatM_free(L, ci);
        L->nci--;
    }
}


//
// free half of the CallInfo structures not in use by a coroutine
//
void lhatE_shrinkCI(lhat_State *L)
{
    CallInfo *ci = L->ci;
    CallInfo *next2;  // next's next
    // while there are two nexts
    while(ci->next != NULL && (next2 = ci->next->next) != NULL) {
        lhatM_free(L, ci->next);  // free next
        L->nci--;
        ci->next = next2;  // remove 'next' from the list
        next2->previous = ci;
        ci = next2;  // keep next's next
    }
}


static void stack_init(lhat_State *L1, lhat_State *L)
{
    // initialize stack array
    L1->stack = lhatM_newvector(L, BASIC_STACK_SIZE, TValue);
    L1->stacksize = BASIC_STACK_SIZE;
    for(int i = 0; i < BASIC_STACK_SIZE; i++)
        setnilvalue(L1->stack + i);  // erase new stack
    L1->top = L1->stack;
    L1->stack_last = L1->stack + L1->stacksize - EXTRA_STACK;
    // initialize first ci
    CallInfo *ci = &L1->base_ci;
    ci->next = ci->previous = NULL;
    ci->callstatus = 0;
    ci->func = L1->top;
    setnilvalue(L1->top++);  // 'function' entry for this 'ci'
    ci->top = L1->top + LHAT_MINSTACK;
    L1->ci = ci;
}


static void freestack(lhat_State *L)
{
    if(L->stack == NULL)
        return;  // stack not completely built yet
    L->ci = &L->base_ci;  // free the entire 'ci' list
    lhatE_freeCI(L);
    lhat_assert(L->nci == 0);
    lhatM_freearray(L, L->stack, L->stacksize);  // free stack array
}


//
// Create registry table and its predefined values
//
static void init_registry(lhat_State *L, global_State *g)
{
    // create registry
    Table *registry = lhatH_new(L);
    sethvalue(L, &g->l_registry, registry);
    lhatH_resize(L, registry, LHAT_RIDX_LAST, 0);
    // registry[LHAT_RIDX_MAINCOROUTINE] = L
    TValue temp;
    setthvalue(L, &temp, L);  // temp = L
    lhatH_setint(L, registry, LHAT_RIDX_MAINSTATE, &temp);
    // registry[LHAT_RIDX_GLOBALS] = table of globals
    sethvalue(L, &temp, lhatH_new(L));  // temp = new table (global table)
    lhatH_setint(L, registry, LHAT_RIDX_GLOBALS, &temp);
}


//
// open parts of the state that may cause memory-allocation errors.
// ('g->version' != NULL flags that the state was completely build)
//
static void f_lhatopen(lhat_State *L, void *ud)
{
    global_State *g = G(L);
    UNUSED(ud);
    stack_init(L, L);  // init stack
    init_registry(L, g);
    lhatS_init(L);
    lhatT_init(L);
    lhatX_init(L);
    g->gcrunning = 1;  // allow gc
    g->version = lhat_version(NULL);
    lhati_userstateopen(L);
}


//
// preinitialize a coroutine with consistent values without allocating
// any memory (to avoid errors)
//
static void preinit_coroutine(lhat_State *L, global_State *g)
{
    G(L) = g;
    L->stack = NULL;
    L->ci = NULL;
    L->nci = 0;
    L->stacksize = 0;
    L->twups = L;  // coroutine has no upvalues
    L->errorJmp = NULL;
    L->nCcalls = 0;
    L->hook = NULL;
    L->hookmask = 0;
    L->basehookcount = 0;
    L->allowhook = 1;
    resethookcount(L);
    L->openupval = NULL;
    L->nny = 1;
    L->status = LHAT_OK;
    L->errfunc = 0;
}


static void close_state(lhat_State *L)
{
    global_State *g = G(L);
    lhatF_close(L, L->stack);  // close all upvalues for this coroutine
    lhatC_freeallobjects(L);  // collect all objects
    if(g->version)  // closing a fully built state?
        lhati_userstateclose(L);
    lhatM_freearray(L, G(L)->strt.hash, G(L)->strt.size);
    freestack(L);
    lhat_assert(gettotalbytes(g) == sizeof(LG));
    (*g->frealloc)(g->ud, fromstate(L), sizeof(LG), 0);  // free main block
}


LHAT_API lhat_State *lhat_newcoroutine(lhat_State *L)
{
    global_State *g = G(L);
    lhat_lock(L);
    lhatC_checkGC(L);
    // create new coroutine
    lhat_State *L1 = &cast(LX *, lhatM_newobject(L, LHAT_TCOROUTINE, sizeof(LX)))->l;
    L1->marked = lhatC_white(g);
    L1->tt = LHAT_TCOROUTINE;
    // link it on list 'allgc'
    L1->next = g->allgc;
    g->allgc = obj2gco(L1);
    // anchor it on L stack
    setthvalue(L, L->top, L1);
    api_incr_top(L);
    preinit_coroutine(L1, g);
    L1->hookmask = L->hookmask;
    L1->basehookcount = L->basehookcount;
    L1->hook = L->hook;
    resethookcount(L1);
    // initialize L1 extra space
    memcpy(lhat_getextraspace(L1), lhat_getextraspace(g->maincoroutine), LHAT_EXTRASPACE);
    lhati_userstatecoroutine(L, L1);
    stack_init(L1, L);  // init stack
    lhat_unlock(L);
    return L1;
}


void lhatE_freecoroutine(lhat_State *L, lhat_State *L1)
{
    LX *l = fromstate(L1);
    lhatF_close(L1, L1->stack);  // close all upvalues for this coroutine
    lhat_assert(L1->openupval == NULL);
    lhati_userstatefree(L, L1);
    freestack(L1);
    lhatM_free(L, l);
}


LHAT_API lhat_State *lhat_newstate(lhat_Alloc f, void *ud)
{
    LG *l = cast(LG *, (*f)(ud, NULL, LHAT_TCOROUTINE, sizeof(LG)));
    if(l == NULL) return NULL;
    lhat_State *L = &l->l.l;
    global_State *g = &l->g;
    L->next = NULL;
    L->tt = LHAT_TCOROUTINE;
    g->currentwhite = bitmask(WHITE0BIT);
    L->marked = lhatC_white(g);
    preinit_coroutine(L, g);
    g->frealloc = f;
    g->ud = ud;
    g->maincoroutine = L;
    g->seed = makeseed(L);
    g->gcrunning = 0;  // no GC while building state
    g->GCestimate = 0;
    g->strt.size = g->strt.nuse = 0;
    g->strt.hash = NULL;
    setnilvalue(&g->l_registry);
    g->panic = NULL;
    g->version = NULL;
    g->gcstate = GCSpause;
    g->gckind = KGC_NORMAL;
    g->allgc = g->finobj = g->tobefnz = g->fixedgc = NULL;
    g->sweepgc = NULL;
    g->gray = g->grayagain = NULL;
    g->weak = g->ephemeron = g->allweak = NULL;
    g->twups = NULL;
    g->totalbytes = sizeof(LG);
    g->GCdebt = 0;
    g->gcfinnum = 0;
    g->gcpause = LHATI_GCPAUSE;
    g->gcstepmul = LHATI_GCMUL;
    for(int i = 0; i < LHAT_NUMTAGS; i++) g->mt[i] = NULL;
    if(lhatD_rawrunprotected(L, f_lhatopen, NULL) != LHAT_OK) {
        // memory allocation error: free partial state
        close_state(L);
        L = NULL;
    }
    return L;
}


LHAT_API void lhat_close(lhat_State *L)
{
    L = G(L)->maincoroutine;  // only the main coroutine can be closed
    lhat_lock(L);
    close_state(L);
}
