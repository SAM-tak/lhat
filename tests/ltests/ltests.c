//
// Internal Module for Debugging of the L^ Implementation
// See Copyright Notice in lhat.h
//
#define LHAT_CORE

#include "lprefix.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lapi.h"
#include "lauxlib.h"
#include "lcode.h"
#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lhatlib.h"

//
// The whole module only makes sense with LHAT_DEBUG on
//
#if defined(LHAT_DEBUG)


void *l_Trick = 0;


int islocked = 0;


#define obj_at(L,k)	(L->ci->func + (k))


static int runC(lhat_State *L, lhat_State *L1, const char *pc);


static void setnameval(lhat_State *L, const char *name, int val)
{
    lhat_pushstring(L, name);
    lhat_pushinteger(L, val);
    lhat_settable(L, -3);
}


static void pushobject(lhat_State *L, const TValue *o)
{
    setobj2s(L, L->top, o);
    api_incr_top(L);
}


static int tpanic(lhat_State *L)
{
    fprintf(stderr, "PANIC: unprotected error in call to L^ API (%s)\n", lhat_tostring(L, -1));
    return (exit(EXIT_FAILURE), 0);  // do not return to L^
}


//
// {======================================================================
// Controlled version for realloc.
// =======================================================================
//

#define MARK		0x55  // 01010101 (a nice pattern)

typedef union Header {
    L_Umaxalign a;  // ensures maximum alignment for Header
    struct {
        size_t size;
        int type;
    } d;
} Header;


#if !defined(EXTERNMEMCHECK)

// full memory check
#define MARKSIZE	16  // size of marks after each block
#define fillmem(mem,size)	memset(mem, -MARK, size)

#else

// external memory check: don't do it twice
#define MARKSIZE	0
#define fillmem(mem,size)	// empty

#endif


Memcontrol l_memcontrol = { 0L, 0L, 0L, 0L, { 0L, 0L, 0L, 0L, 0L, 0L, 0L, 0L, 0L } };


static void freeblock(Memcontrol *mc, Header *block)
{
    if(block) {
        size_t size = block->d.size;
        for(int i = 0; i < MARKSIZE; i++) { // check marks after block
            lhat_assert(*(cast(char *, block + 1) + size + i) == MARK);
        }
        mc->objcount[block->d.type]--;
        fillmem(block, sizeof(Header) + size + MARKSIZE);  // erase block
        free(block);  // actually free block
        mc->numblocks--;  // update counts
        mc->total -= size;
    }
}


void *debug_realloc(void *ud, void *b, size_t oldsize, size_t size)
{
    Memcontrol *mc = cast(Memcontrol *, ud);
    Header *block = cast(Header *, b);
    int type;
    if(mc->memlimit == 0) {  // first time?
        char *limit = getenv("MEMLIMIT");  // initialize memory limit
        mc->memlimit = limit ? strtoul(limit, NULL, 10) : ULONG_MAX;
    }
    if(block == NULL) {
        type = (oldsize < LHAT_NUMTAGS) ? oldsize : 0;
        oldsize = 0;
    }
    else {
        block--;  // go to real header
        type = block->d.type;
        lhat_assert(oldsize == block->d.size);
    }
    if(size == 0) {
        freeblock(mc, block);
        return NULL;
    }
    else if(size > oldsize && mc->total + size - oldsize > mc->memlimit)
        return NULL;  // fake a memory allocation error
    else {
        Header *newblock;
        int i;
        size_t commonsize = (oldsize < size) ? oldsize : size;
        size_t realsize = sizeof(Header) + size + MARKSIZE;
        if(realsize < size) return NULL;  // arithmetic overflow!
        newblock = cast(Header *, malloc(realsize));  // alloc a new block
        if(newblock == NULL) return NULL;  // really out of memory?
        if(block) {
            memcpy(newblock + 1, block + 1, commonsize);  // copy old contents
            freeblock(mc, block);  // erase (and check) old copy
        }
        // initialize new part of the block with something weird
        fillmem(cast(char *, newblock + 1) + commonsize, size - commonsize);
        // initialize marks after block
        for(i = 0; i < MARKSIZE; i++)
            *(cast(char *, newblock + 1) + size + i) = MARK;
        newblock->d.size = size;
        newblock->d.type = type;
        mc->total += size;
        if(mc->total > mc->maxmem)
            mc->maxmem = mc->total;
        mc->numblocks++;
        mc->objcount[type]++;
        return newblock + 1;
    }
}


// }======================================================================



//
// {======================================================
// Functions to check memory consistency
// =======================================================
//


static int testobjref1(global_State *g, GCObject *f, GCObject *t)
{
    if(isdead(g, t)) return 0;
    if(!issweepphase(g))
        return !(isblack(f) && iswhite(t));
    else return 1;
}


static void printobj(global_State *g, GCObject *o)
{
    printf("||%s(%p)-%c(%02X)||",
        ttypename(novariant(o->tt)), (void *)o,
        isdead(g, o) ? 'd' : isblack(o) ? 'b' : iswhite(o) ? 'w' : 'g', o->marked);
}


static int testobjref(global_State *g, GCObject *f, GCObject *t)
{
    int r1 = testobjref1(g, f, t);
    if(!r1) {
        printf("%d(%02X) - ", g->gcstate, g->currentwhite);
        printobj(g, f);
        printf("  ->  ");
        printobj(g, t);
        printf("\n");
    }
    return r1;
}

#define checkobjref(g,f,t)  \
	{ if (t) lhat_longassert(testobjref(g,f,obj2gco(t))); }


static void checkvalref(global_State *g, GCObject *f, const TValue *t)
{
    lhat_assert(!iscollectable(t) || (righttt(t) && testobjref(g, f, gcvalue(t))));
}


static void checktable(global_State *g, Table *h)
{
    unsigned int i;
    Node *n, *limit = gnode(h, sizenode(h));
    GCObject *hgc = obj2gco(h);
    checkobjref(g, hgc, h->metatable);
    for(i = 0; i < h->sizearray; i++)
        checkvalref(g, hgc, &h->array[i]);
    for(n = gnode(h, 0); n < limit; n++) {
        if(!ttisnil(gval(n))) {
            lhat_assert(!ttisnil(gkey(n)));
            checkvalref(g, hgc, gkey(n));
            checkvalref(g, hgc, gval(n));
        }
    }
}


//
// All marks are conditional because a GC may happen while the
// prototype is still being created
//
static void checkproto(global_State *g, Proto *f)
{
    GCObject *fgc = obj2gco(f);
    checkobjref(g, fgc, f->cache);
    checkobjref(g, fgc, f->source);
    for(int i = 0; i<f->sizek; i++) {
        if(ttisstring(f->k + i))
            checkobjref(g, fgc, tsvalue(f->k + i));
    }
    for(int i = 0; i<f->sizeupvalues; i++)
        checkobjref(g, fgc, f->upvalues[i].name);
    for(int i = 0; i<f->sizep; i++)
        checkobjref(g, fgc, f->p[i]);
    for(int i = 0; i<f->sizelocvars; i++)
        checkobjref(g, fgc, f->locvars[i].varname);
}


static void checkCclosure(global_State *g, CClosure *cl)
{
    GCObject *clgc = obj2gco(cl);
    for(int i = 0; i < cl->nupvalues; i++)
        checkvalref(g, clgc, &cl->upvalues[i]);
}


static void checkLclosure(global_State *g, LClosure *cl)
{
    GCObject *clgc = obj2gco(cl);
    checkobjref(g, clgc, cl->p);
    for(int i = 0; i<cl->nupvalues; i++) {
        Upvalue *uv = cl->upvalues[i];
        if(uv) {
            if(!upisopen(uv))  // only closed upvalues matter to invariant
                checkvalref(g, clgc, uv->v);
            lhat_assert(uv->refcount > 0);
        }
    }
}


static int lhat_checkpc(lhat_State *L, CallInfo *ci)
{
    if(!isLhat(ci)) return 1;
    else {
        // if function yielded (inside a hook), real 'func' is in 'extra' field
        StkId f = (L->status != LHAT_YIELD || ci != L->ci)
            ? ci->func
            : restorestack(L, ci->extra);
        Proto *p = clLvalue(f)->p;
        return p->code <= ci->u.l.savedpc &&
            ci->u.l.savedpc <= p->code + p->sizecode;
    }
}


static void checkstack(global_State *g, lhat_State *L1)
{
    lhat_assert(!isdead(g, L1));
    for(Upvalue *uv = L1->openupval; uv != NULL; uv = uv->u.open.next)
        lhat_assert(upisopen(uv));  // must be open
    for(CallInfo *ci = L1->ci; ci != NULL; ci = ci->previous) {
        lhat_assert(ci->top <= L1->stack_last);
        lhat_assert(lhat_checkpc(L1, ci));
    }
    if(L1->stack) {  // complete coroutine?
        for(StkId o = L1->stack; o < L1->stack_last + EXTRA_STACK; o++)
            checkliveness(L1, o);  // entire stack must have valid values
    }
    else lhat_assert(L1->stacksize == 0);
}


static void checkobject(global_State *g, GCObject *o, int maybedead)
{
    if(isdead(g, o))
        lhat_assert(maybedead);
    else {
        lhat_assert(g->gcstate != GCSpause || iswhite(o));
        switch(o->tt) {
        case LHAT_TUSERDATA: {
            TValue uservalue;
            Table *mt = gco2u(o)->metatable;
            checkobjref(g, o, mt);
            getuservalue(g->maincoroutine, gco2u(o), &uservalue);
            checkvalref(g, o, &uservalue);
            break;
        }
        case LHAT_TTABLE: {
            checktable(g, gco2t(o));
            break;
        }
        case LHAT_TCOROUTINE: {
            checkstack(g, gco2th(o));
            break;
        }
        case LHAT_TLCL: {
            checkLclosure(g, gco2lcl(o));
            break;
        }
        case LHAT_TCCL: {
            checkCclosure(g, gco2ccl(o));
            break;
        }
        case LHAT_TPROTO: {
            checkproto(g, gco2p(o));
            break;
        }
        case LHAT_TSHRSTR:
        case LHAT_TLNGSTR: {
            lhat_assert(!isgray(o));  // strings are never gray
            break;
        }
        default: lhat_assert(0);
        }
    }
}


#define TESTGRAYBIT		7

static void checkgraylist(global_State *g, GCObject *o)
{
    ((void)g);  // better to keep it available if we need to print an object
    while(o) {
        lhat_assert(isgray(o));
        lhat_assert(!testbit(o->marked, TESTGRAYBIT));
        l_setbit(o->marked, TESTGRAYBIT);
        switch(o->tt) {
        case LHAT_TTABLE: o = gco2t(o)->gclist; break;
        case LHAT_TLCL: o = gco2lcl(o)->gclist; break;
        case LHAT_TCCL: o = gco2ccl(o)->gclist; break;
        case LHAT_TCOROUTINE: o = gco2th(o)->gclist; break;
        case LHAT_TPROTO: o = gco2p(o)->gclist; break;
        default: lhat_assert(0);  // other objects cannot be gray
        }
    }
}


//
// mark all objects in gray lists with the TESTGRAYBIT, so that
// 'checkmemory' can check that all gray objects are in a gray list
//
static void markgrays(global_State *g)
{
    if(!keepinvariant(g)) return;
    checkgraylist(g, g->gray);
    checkgraylist(g, g->grayagain);
    checkgraylist(g, g->weak);
    checkgraylist(g, g->ephemeron);
    checkgraylist(g, g->allweak);
}


static void checkgray(global_State *g, GCObject *o)
{
    for(; o != NULL; o = o->next) {
        if(isgray(o)) {
            lhat_assert(!keepinvariant(g) || testbit(o->marked, TESTGRAYBIT));
            resetbit(o->marked, TESTGRAYBIT);
        }
        lhat_assert(!testbit(o->marked, TESTGRAYBIT));
    }
}


int lhat_checkmemory(lhat_State *L)
{
    global_State *g = G(L);
    if(keepinvariant(g)) {
        lhat_assert(!iswhite(g->maincoroutine));
        lhat_assert(!iswhite(gcvalue(&g->l_registry)));
    }
    lhat_assert(!isdead(g, gcvalue(&g->l_registry)));
    checkstack(g, g->maincoroutine);
    resetbit(g->maincoroutine->marked, TESTGRAYBIT);
    lhat_assert(g->sweepgc == NULL || issweepphase(g));
    markgrays(g);
    // check 'fixedgc' list
    for(GCObject *o = g->fixedgc; o != NULL; o = o->next) {
        lhat_assert(o->tt == LHAT_TSHRSTR && isgray(o));
    }
    // check 'allgc' list
    checkgray(g, g->allgc);
    int maybedead = (GCSatomic < g->gcstate && g->gcstate <= GCSswpallgc);
    for(GCObject *o = g->allgc; o != NULL; o = o->next) {
        checkobject(g, o, maybedead);
        lhat_assert(!tofinalize(o));
    }
    // check 'finobj' list
    checkgray(g, g->finobj);
    for(GCObject *o = g->finobj; o != NULL; o = o->next) {
        checkobject(g, o, 0);
        lhat_assert(tofinalize(o));
        lhat_assert(o->tt == LHAT_TUSERDATA || o->tt == LHAT_TTABLE);
    }
    // check 'tobefnz' list
    checkgray(g, g->tobefnz);
    for(GCObject *o = g->tobefnz; o != NULL; o = o->next) {
        checkobject(g, o, 0);
        lhat_assert(tofinalize(o));
        lhat_assert(o->tt == LHAT_TUSERDATA || o->tt == LHAT_TTABLE);
    }
    return 0;
}

// }======================================================



//
// {======================================================
// Disassembler
// =======================================================
//


static char *buildop(Proto *p, int pc, char *buff)
{
    Instruction i = p->code[pc];
    OpCode o = GET_OPCODE(i);
    const char *name = lhatP_opnames[o];
    int line = getfuncline(p, pc);
    sprintf(buff, "(%4d) %4d - ", line, pc);
    switch(getOpMode(o)) {
    case iABC:
        sprintf(buff + strlen(buff), "%-12s%4d %4d %4d", name, GETARG_A(i), GETARG_B(i), GETARG_C(i));
        break;
    case iABx:
        sprintf(buff + strlen(buff), "%-12s%4d %4d", name, GETARG_A(i), GETARG_Bx(i));
        break;
    case iAsBx:
        sprintf(buff + strlen(buff), "%-12s%4d %4d", name, GETARG_A(i), GETARG_sBx(i));
        break;
    case iAx:
        sprintf(buff + strlen(buff), "%-12s%4d", name, GETARG_Ax(i));
        break;
    }
    return buff;
}


#if 0
void lhatI_printcode(Proto *pt, int size)
{
    int pc;
    for(pc = 0; pc<size; pc++) {
        char buff[100];
        printf("%s\n", buildop(pt, pc, buff));
    }
    printf("-------\n");
}


void lhatI_printinst(Proto *pt, int pc)
{
    char buff[100];
    printf("%s\n", buildop(pt, pc, buff));
}
#endif


static int listcode(lhat_State *L)
{
    lhatL_argcheck(L, lhat_isfunction(L, 1) && !lhat_iscfunction(L, 1), 1, "Lhat function expected");
    Proto *p = getproto(obj_at(L, 1));
    lhat_newtable(L);
    setnameval(L, "maxstack", p->maxstacksize);
    setnameval(L, "numparams", p->numparams);
    for(int pc = 0; pc<p->sizecode; pc++) {
        char buff[100];
        lhat_pushinteger(L, pc + 1);
        lhat_pushstring(L, buildop(p, pc, buff));
        lhat_settable(L, -3);
    }
    return 1;
}


static int listk(lhat_State *L)
{
    lhatL_argcheck(L, lhat_isfunction(L, 1) && !lhat_iscfunction(L, 1), 1, "Lhat function expected");
    Proto *p = getproto(obj_at(L, 1));
    lhat_createtable(L, p->sizek, 0);
    for(int i = 0; i<p->sizek; i++) {
        pushobject(L, p->k + i);
        lhat_rawseti(L, -2, i + 1);
    }
    return 1;
}


static int listlocals(lhat_State *L)
{
    int pc = cast_int(lhatL_checkinteger(L, 2)) - 1;
    int i = 0;
    lhatL_argcheck(L, lhat_isfunction(L, 1) && !lhat_iscfunction(L, 1), 1, "Lhat function expected");
    Proto *p = getproto(obj_at(L, 1));
    const char *name;
    while((name = lhatF_getlocalname(p, ++i, pc)) != NULL)
        lhat_pushstring(L, name);
    return i - 1;
}

// }======================================================



static void printstack(lhat_State *L)
{
    int i;
    int n = lhat_gettop(L);
    for(i = 1; i <= n; i++) {
        printf("%3d: %s\n", i, lhatL_tolstring(L, i, NULL));
        lhat_pop(L, 1);
    }
    printf("\n");
}


static int get_limits(lhat_State *L)
{
    lhat_createtable(L, 0, 5);
    setnameval(L, "BITS_INT", LHATI_BITSINT);
    setnameval(L, "MAXARG_Ax", MAXARG_Ax);
    setnameval(L, "MAXARG_Bx", MAXARG_Bx);
    setnameval(L, "MAXARG_sBx", MAXARG_sBx);
    setnameval(L, "BITS_INT", LHATI_BITSINT);
    setnameval(L, "LFPF", LFIELDS_PER_FLUSH);
    setnameval(L, "NUM_OPCODES", NUM_OPCODES);
    return 1;
}


static int mem_query(lhat_State *L)
{
    if(lhat_isnone(L, 1)) {
        lhat_pushinteger(L, l_memcontrol.total);
        lhat_pushinteger(L, l_memcontrol.numblocks);
        lhat_pushinteger(L, l_memcontrol.maxmem);
        return 3;
    }
    else if(lhat_isnumber(L, 1)) {
        unsigned long limit = cast(unsigned long, lhatL_checkinteger(L, 1));
        if(limit == 0) limit = ULONG_MAX;
        l_memcontrol.memlimit = limit;
        return 0;
    }
    else {
        const char *t = lhatL_checkstring(L, 1);
        int i;
        for(i = LHAT_NUMTAGS - 1; i >= 0; i--) {
            if(strcmp(t, ttypename(i)) == 0) {
                lhat_pushinteger(L, l_memcontrol.objcount[i]);
                return 1;
            }
        }
        return lhatL_error(L, "unkown type '%s'", t);
    }
}


static int settrick(lhat_State *L)
{
    if(ttisnil(obj_at(L, 1)))
        l_Trick = NULL;
    else
        l_Trick = gcvalue(obj_at(L, 1));
    return 0;
}


static int gc_color(lhat_State *L)
{
    TValue *o;
    lhatL_checkany(L, 1);
    o = obj_at(L, 1);
    if(!iscollectable(o))
        lhat_pushstring(L, "no collectable");
    else {
        GCObject *obj = gcvalue(o);
        lhat_pushstring(L, isdead(G(L), obj) ? "dead" :
            iswhite(obj) ? "white" :
            isblack(obj) ? "black" : "grey");
    }
    return 1;
}


static int gc_state(lhat_State *L)
{
    static const char *statenames[] = { "propagate", "atomic", "sweepallgc",
        "sweepfinobj", "sweeptobefnz", "sweepend", "pause", "" };
    static const int states[] = { GCSpropagate, GCSatomic, GCSswpallgc,
        GCSswpfinobj, GCSswptobefnz, GCSswpend, GCSpause, -1 };
    int option = states[lhatL_checkoption(L, 1, "", statenames)];
    if(option == -1) {
        lhat_pushstring(L, statenames[G(L)->gcstate]);
        return 1;
    }
    else {
        global_State *g = G(L);
        lhat_lock(L);
        if(option < g->gcstate) {  // must cross 'pause'?
            lhatC_runtilstate(L, bitmask(GCSpause));  // run until pause
        }
        lhatC_runtilstate(L, bitmask(option));
        lhat_assert(G(L)->gcstate == option);
        lhat_unlock(L);
        return 0;
    }
}


static int hash_query(lhat_State *L)
{
    if(lhat_isnone(L, 2)) {
        lhatL_argcheck(L, lhat_type(L, 1) == LHAT_TSTRING, 1, "string expected");
        lhat_pushinteger(L, tsvalue(obj_at(L, 1))->hash);
    }
    else {
        TValue *o = obj_at(L, 1);
        Table *t;
        lhatL_checktype(L, 2, LHAT_TTABLE);
        t = hvalue(obj_at(L, 2));
        lhat_pushinteger(L, lhatH_mainposition(t, o) - t->node);
    }
    return 1;
}


static int stacklevel(lhat_State *L)
{
    unsigned long a = 0;
    lhat_pushinteger(L, (L->top - L->stack));
    lhat_pushinteger(L, (L->stack_last - L->stack));
    lhat_pushinteger(L, (unsigned long)&a);
    return 3;
}


static int table_query(lhat_State *L)
{
    const Table *t;
    int i = cast_int(lhatL_optinteger(L, 2, -1));
    lhatL_checktype(L, 1, LHAT_TTABLE);
    t = hvalue(obj_at(L, 1));
    if(i == -1) {
        lhat_pushinteger(L, t->sizearray);
        lhat_pushinteger(L, allocsizenode(t));
        lhat_pushinteger(L, isdummy(t) ? 0 : t->lastfree - t->node);
    }
    else if((unsigned int)i < t->sizearray) {
        lhat_pushinteger(L, i);
        pushobject(L, &t->array[i]);
        lhat_pushnil(L);
    }
    else if((i -= t->sizearray) < sizenode(t)) {
        if(!ttisnil(gval(gnode(t, i))) ||
            ttisnil(gkey(gnode(t, i))) ||
            ttisnumber(gkey(gnode(t, i)))) {
            pushobject(L, gkey(gnode(t, i)));
        }
        else
            lhat_pushliteral(L, "<undef>");
        pushobject(L, gval(gnode(t, i)));
        if(gnext(&t->node[i]) != 0)
            lhat_pushinteger(L, gnext(&t->node[i]));
        else
            lhat_pushnil(L);
    }
    return 3;
}


static int string_query(lhat_State *L)
{
    stringtable *tb = &G(L)->strt;
    int s = cast_int(lhatL_optinteger(L, 1, 0)) - 1;
    if(s == -1) {
        lhat_pushinteger(L, tb->size);
        lhat_pushinteger(L, tb->nuse);
        return 2;
    }
    else if(s < tb->size) {
        TString *ts;
        int n = 0;
        for(ts = tb->hash[s]; ts != NULL; ts = ts->u.hnext) {
            setsvalue2s(L, L->top, ts);
            api_incr_top(L);
            n++;
        }
        return n;
    }
    else return 0;
}


static int tref(lhat_State *L)
{
    int level = lhat_gettop(L);
    lhatL_checkany(L, 1);
    lhat_pushvalue(L, 1);
    lhat_pushinteger(L, lhatL_ref(L, LHAT_REGISTRYINDEX));
    lhat_assert(lhat_gettop(L) == level + 1);  // +1 for result
    return 1;
}

static int getref(lhat_State *L)
{
    int level = lhat_gettop(L);
    lhat_rawgeti(L, LHAT_REGISTRYINDEX, lhatL_checkinteger(L, 1));
    lhat_assert(lhat_gettop(L) == level + 1);
    return 1;
}

static int unref(lhat_State *L)
{
    int level = lhat_gettop(L);
    lhatL_unref(L, LHAT_REGISTRYINDEX, cast_int(lhatL_checkinteger(L, 1)));
    lhat_assert(lhat_gettop(L) == level);
    return 0;
}


static int upvalues(lhat_State *L)
{
    int n = cast_int(lhatL_checkinteger(L, 2));
    lhatL_checktype(L, 1, LHAT_TFUNCTION);
    if(lhat_isnone(L, 3)) {
        const char *name = lhat_getupvalue(L, 1, n);
        if(name == NULL) return 0;
        lhat_pushstring(L, name);
        return 2;
    }
    else {
        const char *name = lhat_setupvalue(L, 1, n);
        lhat_pushstring(L, name);
        return 1;
    }
}


static int newuserdata(lhat_State *L)
{
    size_t size = cast(size_t, lhatL_checkinteger(L, 1));
    char *p = cast(char *, lhat_newuserdata(L, size));
    while(size--) *p++ = '\0';
    return 1;
}


static int pushuserdata(lhat_State *L)
{
    lhat_Integer u = lhatL_checkinteger(L, 1);
    lhat_pushlightuserdata(L, cast(void *, cast(size_t, u)));
    return 1;
}


static int udataval(lhat_State *L)
{
    lhat_pushinteger(L, cast(long, lhat_touserdata(L, 1)));
    return 1;
}


static int doonnewstack(lhat_State *L)
{
    lhat_State *L1 = lhat_newcoroutine(L);
    size_t l;
    const char *s = lhatL_checklstring(L, 1, &l);
    int status = lhatL_loadbuffer(L1, s, l, s);
    if(status == LHAT_OK)
        status = lhat_pcall(L1, 0, 0, 0);
    lhat_pushinteger(L, status);
    return 1;
}


static int s2d(lhat_State *L)
{
    lhat_pushnumber(L, *cast(const double *, lhatL_checkstring(L, 1)));
    return 1;
}


static int d2s(lhat_State *L)
{
    double d = lhatL_checknumber(L, 1);
    lhat_pushlstring(L, cast(char *, &d), sizeof(d));
    return 1;
}


static int num2int(lhat_State *L)
{
    lhat_pushinteger(L, lhat_tointeger(L, 1));
    return 1;
}


static int newstate(lhat_State *L)
{
    void *ud;
    lhat_Alloc f = lhat_getallocf(L, &ud);
    lhat_State *L1 = lhat_newstate(f, ud);
    if(L1) {
        lhat_atpanic(L1, tpanic);
        lhat_pushlightuserdata(L, L1);
    }
    else
        lhat_pushnil(L);
    return 1;
}


static lhat_State *getstate(lhat_State *L)
{
    lhat_State *L1 = cast(lhat_State *, lhat_touserdata(L, 1));
    lhatL_argcheck(L, L1 != NULL, 1, "state expected");
    return L1;
}


static int loadlib(lhat_State *L)
{
    static const lhatL_Reg libs[] = {
        { "_G", lhatopen_base },
        { "coroutine", lhatopen_coroutine },
        { "debug", lhatopen_debug },
        { "io", lhatopen_io },
        { "os", lhatopen_os },
        { "math", lhatopen_math },
        { "string", lhatopen_string },
        { "table", lhatopen_table },
        { NULL, NULL }
    };
    lhat_State *L1 = getstate(L);
    int i;
    lhatL_requiref(L1, "package", lhatopen_package, 0);
    lhat_assert(lhat_type(L1, -1) == LHAT_TTABLE);
    // 'requiref' should not reload module already loaded...
    lhatL_requiref(L1, "package", NULL, 1);  // seg. fault if it reloads
                                             // ...but should return the same module
    lhat_assert(lhat_compare(L1, -1, -2, LHAT_OPEQ));
    lhatL_getsubtable(L1, LHAT_REGISTRYINDEX, LHAT_PRELOAD_TABLE);
    for(i = 0; libs[i].name; i++) {
        lhat_pushcfunction(L1, libs[i].func);
        lhat_setfield(L1, -2, libs[i].name);
    }
    return 0;
}

static int closestate(lhat_State *L)
{
    lhat_State *L1 = getstate(L);
    lhat_close(L1);
    return 0;
}

static int doremote(lhat_State *L)
{
    lhat_State *L1 = getstate(L);
    size_t lcode;
    const char *code = lhatL_checklstring(L, 2, &lcode);
    int status;
    lhat_settop(L1, 0);
    status = lhatL_loadbuffer(L1, code, lcode, code);
    if(status == LHAT_OK)
        status = lhat_pcall(L1, 0, LHAT_MULTRET, 0);
    if(status != LHAT_OK) {
        lhat_pushnil(L);
        lhat_pushstring(L, lhat_tostring(L1, -1));
        lhat_pushinteger(L, status);
        return 3;
    }
    else {
        int i = 0;
        while(!lhat_isnone(L1, ++i))
            lhat_pushstring(L, lhat_tostring(L1, i));
        lhat_pop(L1, i - 1);
        return i - 1;
    }
}


static int int2fb_aux(lhat_State *L)
{
    int b = lhatO_int2fb((unsigned int)lhatL_checkinteger(L, 1));
    lhat_pushinteger(L, b);
    lhat_pushinteger(L, (unsigned int)lhatO_fb2int(b));
    return 2;
}


static int log2_aux(lhat_State *L)
{
    unsigned int x = (unsigned int)lhatL_checkinteger(L, 1);
    lhat_pushinteger(L, lhatO_ceillog2(x));
    return 1;
}


struct Aux { jmp_buf jb; const char *paniccode; lhat_State *L; };

//
// does a long-jump back to "main program".
//
static int panicback(lhat_State *L)
{
    struct Aux *b;
    lhat_checkstack(L, 1);  // open space for 'Aux' struct
    lhat_getfield(L, LHAT_REGISTRYINDEX, "_jmpbuf");  // get 'Aux' struct
    b = (struct Aux *)lhat_touserdata(L, -1);
    lhat_pop(L, 1);  // remove 'Aux' struct
    runC(b->L, L, b->paniccode);  // run optional panic code
    longjmp(b->jb, 1);
    return 1;  // to avoid warnings
}

static int checkpanic(lhat_State *L)
{
    struct Aux b;
    void *ud;
    lhat_State *L1;
    const char *code = lhatL_checkstring(L, 1);
    lhat_Alloc f = lhat_getallocf(L, &ud);
    b.paniccode = lhatL_optstring(L, 2, "");
    b.L = L;
    L1 = lhat_newstate(f, ud);  // create new state
    if(L1 == NULL) {  // error?
        lhat_pushnil(L);
        return 1;
    }
    lhat_atpanic(L1, panicback);  // set its panic function
    lhat_pushlightuserdata(L1, &b);
    lhat_setfield(L1, LHAT_REGISTRYINDEX, "_jmpbuf");  // store 'Aux' struct
    if(setjmp(b.jb) == 0) {  // set jump buffer
        runC(L, L1, code);  // run code unprotected
        lhat_pushliteral(L, "no errors");
    }
    else {  // error handling
            // move error message to original state
        lhat_pushstring(L, lhat_tostring(L1, -1));
    }
    lhat_close(L1);
    return 1;
}



//
// {====================================================================
// function to test the API with C. It interprets a kind of assembler
// language with calls to the API, so the test can be driven by L^ code
// =====================================================================
//


static void sethookaux(lhat_State *L, int mask, int count, const char *code);

static const char *const delimits = " \t\n,;";

static void skip(const char **pc)
{
    for(;;) {
        if(**pc != '\0' && strchr(delimits, **pc)) (*pc)++;
        else if(**pc == '#') {
            while(**pc != '\n' && **pc != '\0') (*pc)++;
        }
        else break;
    }
}

static int getnum_aux(lhat_State *L, lhat_State *L1, const char **pc)
{
    int res = 0;
    int sig = 1;
    skip(pc);
    if(**pc == '.') {
        res = cast_int(lhat_tointeger(L1, -1));
        lhat_pop(L1, 1);
        (*pc)++;
        return res;
    }
    else if(**pc == '*') {
        res = lhat_gettop(L1);
        (*pc)++;
        return res;
    }
    else if(**pc == '-') {
        sig = -1;
        (*pc)++;
    }
    if(!lisdigit(cast_uchar(**pc)))
        lhatL_error(L, "number expected (%s)", *pc);
    while(lisdigit(cast_uchar(**pc))) res = res * 10 + (*(*pc)++) - '0';
    return sig*res;
}

static const char *getstring_aux(lhat_State *L, char *buff, const char **pc)
{
    int i = 0;
    skip(pc);
    if(**pc == '"' || **pc == '\'') {  // quoted string?
        int quote = *(*pc)++;
        while(**pc != quote) {
            if(**pc == '\0') lhatL_error(L, "unfinished string in C script");
            buff[i++] = *(*pc)++;
        }
        (*pc)++;
    }
    else {
        while(**pc != '\0' && !strchr(delimits, **pc))
            buff[i++] = *(*pc)++;
    }
    buff[i] = '\0';
    return buff;
}


static int getindex_aux(lhat_State *L, lhat_State *L1, const char **pc)
{
    skip(pc);
    switch(*(*pc)++) {
    case 'R': return LHAT_REGISTRYINDEX;
    case 'G': return lhatL_error(L, "deprecated index 'G'");
    case 'U': return lhat_upvalueindex(getnum_aux(L, L1, pc));
    default: (*pc)--; return getnum_aux(L, L1, pc);
    }
}


static void pushcode(lhat_State *L, int code)
{
    static const char *const codes[] = { "OK", "YIELD", "ERRRUN",
        "ERRSYNTAX", "ERRMEM", "ERRGCMM", "ERRERR" };
    lhat_pushstring(L, codes[code]);
}


#define EQ(s1)	(strcmp(s1, inst) == 0)

#define getnum		(getnum_aux(L, L1, &pc))
#define getstring	(getstring_aux(L, buff, &pc))
#define getindex	(getindex_aux(L, L1, &pc))


static int testC(lhat_State *L);
static int Cfunck(lhat_State *L, int status, lhat_KContext ctx);

//
// arithmetic operation encoding for 'arith' instruction
// LHAT_OPIDIV  -> \
// LHAT_OPSHL   -> <
// LHAT_OPSHR   -> >
// LHAT_OPUNM   -> _
// LHAT_OPBNOT  -> !
//
static const char ops[] = "+-*%^/\\&|~<>_!";

static int runC(lhat_State *L, lhat_State *L1, const char *pc)
{
    char buff[300];
    int status = 0;
    if(pc == NULL) return lhatL_error(L, "attempt to runC null script");
    for(;;) {
        const char *inst = getstring;
        if EQ("") return 0;
        else if EQ("absindex")
        {
            lhat_pushnumber(L1, lhat_absindex(L1, getindex));
        }
        else if EQ("append")
        {
            int t = getindex;
            int i = lhat_rawlen(L1, t);
            lhat_rawseti(L1, t, i + 1);
        }
        else if EQ("arith")
        {
            int op;
            skip(&pc);
            op = strchr(ops, *pc++) - ops;
            lhat_arith(L1, op);
        }
        else if EQ("call")
        {
            int narg = getnum;
            int nres = getnum;
            lhat_call(L1, narg, nres);
        }
        else if EQ("callk")
        {
            int narg = getnum;
            int nres = getnum;
            int i = getindex;
            lhat_callk(L1, narg, nres, i, Cfunck);
        }
        else if EQ("checkstack")
        {
            int sz = getnum;
            const char *msg = getstring;
            if(*msg == '\0')
                msg = NULL;  // to test 'lhatL_checkstack' with no message
            lhatL_checkstack(L1, sz, msg);
        }
        else if EQ("compare")
        {
            const char *opt = getstring;  // EQ, LT, or LE
            int op = (opt[0] == 'E') ? LHAT_OPEQ
                : (opt[1] == 'T') ? LHAT_OPLT : LHAT_OPLE;
            int a = getindex;
            int b = getindex;
            lhat_pushboolean(L1, lhat_compare(L1, a, b, op));
        }
        else if EQ("concat")
        {
            lhat_concat(L1, getnum);
        }
        else if EQ("copy")
        {
            int f = getindex;
            lhat_copy(L1, f, getindex);
        }
        else if EQ("func2num")
        {
            lhat_CFunction func = lhat_tocfunction(L1, getindex);
            lhat_pushnumber(L1, cast(size_t, func));
        }
        else if EQ("getfield")
        {
            int t = getindex;
            lhat_getfield(L1, t, getstring);
        }
        else if EQ("getglobal")
        {
            lhat_getglobal(L1, getstring);
        }
        else if EQ("getmetatable")
        {
            if(lhat_getmetatable(L1, getindex) == 0)
                lhat_pushnil(L1);
        }
        else if EQ("gettable")
        {
            lhat_gettable(L1, getindex);
        }
        else if EQ("gettop")
        {
            lhat_pushinteger(L1, lhat_gettop(L1));
        }
        else if EQ("gsub")
        {
            int a = getnum; int b = getnum; int c = getnum;
            lhatL_gsub(L1, lhat_tostring(L1, a),
                lhat_tostring(L1, b),
                lhat_tostring(L1, c));
        }
        else if EQ("insert")
        {
            lhat_insert(L1, getnum);
        }
        else if EQ("iscfunction")
        {
            lhat_pushboolean(L1, lhat_iscfunction(L1, getindex));
        }
        else if EQ("isfunction")
        {
            lhat_pushboolean(L1, lhat_isfunction(L1, getindex));
        }
        else if EQ("isnil")
        {
            lhat_pushboolean(L1, lhat_isnil(L1, getindex));
        }
        else if EQ("isnull")
        {
            lhat_pushboolean(L1, lhat_isnone(L1, getindex));
        }
        else if EQ("isnumber")
        {
            lhat_pushboolean(L1, lhat_isnumber(L1, getindex));
        }
        else if EQ("isstring")
        {
            lhat_pushboolean(L1, lhat_isstring(L1, getindex));
        }
        else if EQ("istable")
        {
            lhat_pushboolean(L1, lhat_istable(L1, getindex));
        }
        else if EQ("isudataval")
        {
            lhat_pushboolean(L1, lhat_islightuserdata(L1, getindex));
        }
        else if EQ("isuserdata")
        {
            lhat_pushboolean(L1, lhat_isuserdata(L1, getindex));
        }
        else if EQ("len")
        {
            lhat_len(L1, getindex);
        }
        else if EQ("Llen")
        {
            lhat_pushinteger(L1, lhatL_len(L1, getindex));
        }
        else if EQ("loadfile")
        {
            lhatL_loadfile(L1, lhatL_checkstring(L1, getnum));
        }
        else if EQ("loadstring")
        {
            const char *s = lhatL_checkstring(L1, getnum);
            lhatL_loadstring(L1, s);
        }
        else if EQ("newmetatable")
        {
            lhat_pushboolean(L1, lhatL_newmetatable(L1, getstring));
        }
        else if EQ("newtable")
        {
            lhat_newtable(L1);
        }
        else if EQ("newcoroutine")
        {
            lhat_newcoroutine(L1);
        }
        else if EQ("newuserdata")
        {
            lhat_newuserdata(L1, getnum);
        }
        else if EQ("next")
        {
            lhat_next(L1, -2);
        }
        else if EQ("objsize")
        {
            lhat_pushinteger(L1, lhat_rawlen(L1, getindex));
        }
        else if EQ("pcall")
        {
            int narg = getnum;
            int nres = getnum;
            status = lhat_pcall(L1, narg, nres, getnum);
        }
        else if EQ("pcallk")
        {
            int narg = getnum;
            int nres = getnum;
            int i = getindex;
            status = lhat_pcallk(L1, narg, nres, 0, i, Cfunck);
        }
        else if EQ("pop")
        {
            lhat_pop(L1, getnum);
        }
        else if EQ("print")
        {
            int n = getnum;
            if(n != 0) {
                printf("%s\n", lhatL_tolstring(L1, n, NULL));
                lhat_pop(L1, 1);
            }
            else printstack(L1);
        }
        else if EQ("pushbool")
        {
            lhat_pushboolean(L1, getnum);
        }
        else if EQ("pushcclosure")
        {
            lhat_pushcclosure(L1, testC, getnum);
        }
        else if EQ("pushint")
        {
            lhat_pushinteger(L1, getnum);
        }
        else if EQ("pushnil")
        {
            lhat_pushnil(L1);
        }
        else if EQ("pushnum")
        {
            lhat_pushnumber(L1, (lhat_Number)getnum);
        }
        else if EQ("pushstatus")
        {
            pushcode(L1, status);
        }
        else if EQ("pushstring")
        {
            lhat_pushstring(L1, getstring);
        }
        else if EQ("pushupvalueindex")
        {
            lhat_pushinteger(L1, lhat_upvalueindex(getnum));
        }
        else if EQ("pushvalue")
        {
            lhat_pushvalue(L1, getindex);
        }
        else if EQ("rawgeti")
        {
            int t = getindex;
            lhat_rawgeti(L1, t, getnum);
        }
        else if EQ("rawgetp")
        {
            int t = getindex;
            lhat_rawgetp(L1, t, cast(void *, cast(size_t, getnum)));
        }
        else if EQ("rawsetp")
        {
            int t = getindex;
            lhat_rawsetp(L1, t, cast(void *, cast(size_t, getnum)));
        }
        else if EQ("remove")
        {
            lhat_remove(L1, getnum);
        }
        else if EQ("replace")
        {
            lhat_replace(L1, getindex);
        }
        else if EQ("resume")
        {
            int i = getindex;
            status = lhat_resume(lhat_tocoroutine(L1, i), L, getnum);
        }
        else if EQ("return")
        {
            int n = getnum;
            if(L1 != L) {
                int i;
                for(i = 0; i < n; i++)
                    lhat_pushstring(L, lhat_tostring(L1, -(n - i)));
            }
            return n;
        }
        else if EQ("rotate")
        {
            int i = getindex;
            lhat_rotate(L1, i, getnum);
        }
        else if EQ("setfield")
        {
            int t = getindex;
            lhat_setfield(L1, t, getstring);
        }
        else if EQ("setglobal")
        {
            lhat_setglobal(L1, getstring);
        }
        else if EQ("sethook")
        {
            int mask = getnum;
            int count = getnum;
            sethookaux(L1, mask, count, getstring);
        }
        else if EQ("setmetatable")
        {
            lhat_setmetatable(L1, getindex);
        }
        else if EQ("settable")
        {
            lhat_settable(L1, getindex);
        }
        else if EQ("settop")
        {
            lhat_settop(L1, getnum);
        }
        else if EQ("testudata")
        {
            int i = getindex;
            lhat_pushboolean(L1, lhatL_testudata(L1, i, getstring) != NULL);
        }
        else if EQ("error")
        {
            lhat_error(L1);
        }
        else if EQ("throw")
        {
#if defined(__cplusplus)
            static struct X { int x; } x;
            throw x;
#else
            lhatL_error(L1, "C++");
#endif
            break;
        }
        else if EQ("tobool")
        {
            lhat_pushboolean(L1, lhat_toboolean(L1, getindex));
        }
        else if EQ("tocfunction")
        {
            lhat_pushcfunction(L1, lhat_tocfunction(L1, getindex));
        }
        else if EQ("tointeger")
        {
            lhat_pushinteger(L1, lhat_tointeger(L1, getindex));
        }
        else if EQ("tonumber")
        {
            lhat_pushnumber(L1, lhat_tonumber(L1, getindex));
        }
        else if EQ("topointer")
        {
            lhat_pushnumber(L1, cast(size_t, lhat_topointer(L1, getindex)));
        }
        else if EQ("tostring")
        {
            const char *s = lhat_tostring(L1, getindex);
            const char *s1 = lhat_pushstring(L1, s);
            lhat_longassert((s == NULL && s1 == NULL) || strcmp(s, s1) == 0);
        }
        else if EQ("type")
        {
            lhat_pushstring(L1, lhatL_typename(L1, getnum));
        }
        else if EQ("xmove")
        {
            int f = getindex;
            int t = getindex;
            lhat_State *fs = (f == 0) ? L1 : lhat_tocoroutine(L1, f);
            lhat_State *ts = (t == 0) ? L1 : lhat_tocoroutine(L1, t);
            int n = getnum;
            if(n == 0) n = lhat_gettop(fs);
            lhat_xmove(fs, ts, n);
        }
        else if EQ("yield")
        {
            return lhat_yield(L1, getnum);
        }
        else if EQ("yieldk")
        {
            int nres = getnum;
            int i = getindex;
            return lhat_yieldk(L1, nres, i, Cfunck);
        }
        else lhatL_error(L, "unknown instruction %s", buff);
    }
    return 0;
}


static int testC(lhat_State *L)
{
    lhat_State *L1;
    const char *pc;
    if(lhat_isuserdata(L, 1)) {
        L1 = getstate(L);
        pc = lhatL_checkstring(L, 2);
    }
    else if(lhat_iscoroutine(L, 1)) {
        L1 = lhat_tocoroutine(L, 1);
        pc = lhatL_checkstring(L, 2);
    }
    else {
        L1 = L;
        pc = lhatL_checkstring(L, 1);
    }
    return runC(L, L1, pc);
}


static int Cfunc(lhat_State *L)
{
    return runC(L, L, lhat_tostring(L, lhat_upvalueindex(1)));
}


static int Cfunck(lhat_State *L, int status, lhat_KContext ctx)
{
    pushcode(L, status);
    lhat_setglobal(L, "status");
    lhat_pushinteger(L, ctx);
    lhat_setglobal(L, "ctx");
    return runC(L, L, lhat_tostring(L, ctx));
}


static int makeCfunc(lhat_State *L)
{
    lhatL_checkstring(L, 1);
    lhat_pushcclosure(L, Cfunc, lhat_gettop(L));
    return 1;
}


// }======================================================


//
// {======================================================
// tests for C hooks
// =======================================================
//

//
// C hook that runs the C script stored in registry.C_HOOK[L]
//
static void Chook(lhat_State *L, lhat_Debug *ar)
{
    const char *scpt;
    const char *const events[] = { "call", "ret", "line", "count", "tailcall" };
    lhat_getfield(L, LHAT_REGISTRYINDEX, "C_HOOK");
    lhat_pushlightuserdata(L, L);
    lhat_gettable(L, -2);  // get C_HOOK[L] (script saved by sethookaux)
    scpt = lhat_tostring(L, -1);  // not very religious (string will be popped)
    lhat_pop(L, 2);  // remove C_HOOK and script
    lhat_pushstring(L, events[ar->event]);  // may be used by script
    lhat_pushinteger(L, ar->currentline);  // may be used by script
    runC(L, L, scpt);  // run script from C_HOOK[L]
}


//
// sets 'registry.C_HOOK[L] = scpt' and sets 'Chook' as a hook
//
static void sethookaux(lhat_State *L, int mask, int count, const char *scpt)
{
    if(*scpt == '\0') {  // no script?
        lhat_sethook(L, NULL, 0, 0);  // turn off hooks
        return;
    }
    lhat_getfield(L, LHAT_REGISTRYINDEX, "C_HOOK");  // get C_HOOK table
    if(!lhat_istable(L, -1)) {  // no hook table?
        lhat_pop(L, 1);  // remove previous value
        lhat_newtable(L);  // create new C_HOOK table
        lhat_pushvalue(L, -1);
        lhat_setfield(L, LHAT_REGISTRYINDEX, "C_HOOK");  // register it
    }
    lhat_pushlightuserdata(L, L);
    lhat_pushstring(L, scpt);
    lhat_settable(L, -3);  // C_HOOK[L] = script
    lhat_sethook(L, Chook, mask, count);
}


static int sethook(lhat_State *L)
{
    if(lhat_isnoneornil(L, 1))
        lhat_sethook(L, NULL, 0, 0);  // turn off hooks
    else {
        const char *scpt = lhatL_checkstring(L, 1);
        const char *smask = lhatL_checkstring(L, 2);
        int count = cast_int(lhatL_optinteger(L, 3, 0));
        int mask = 0;
        if(strchr(smask, 'c')) mask |= LHAT_MASKCALL;
        if(strchr(smask, 'r')) mask |= LHAT_MASKRET;
        if(strchr(smask, 'l')) mask |= LHAT_MASKLINE;
        if(count > 0) mask |= LHAT_MASKCOUNT;
        sethookaux(L, mask, count, scpt);
    }
    return 0;
}


static int coresume(lhat_State *L)
{
    int status;
    lhat_State *co = lhat_tocoroutine(L, 1);
    lhatL_argcheck(L, co, 1, "coroutine expected");
    status = lhat_resume(co, L, 0);
    if(status != LHAT_OK && status != LHAT_YIELD) {
        lhat_pushboolean(L, 0);
        lhat_insert(L, -2);
        return 2;  // return false + error message
    }
    else {
        lhat_pushboolean(L, 1);
        return 1;
    }
}

// }======================================================



static const struct lhatL_Reg tests_funcs[] = {
    { "checkmemory", lhat_checkmemory },
    { "closestate", closestate },
    { "d2s", d2s },
    { "doonnewstack", doonnewstack },
    { "doremote", doremote },
    { "gccolor", gc_color },
    { "gcstate", gc_state },
    { "getref", getref },
    { "hash", hash_query },
    { "int2fb", int2fb_aux },
    { "log2", log2_aux },
    { "limits", get_limits },
    { "listcode", listcode },
    { "listk", listk },
    { "listlocals", listlocals },
    { "loadlib", loadlib },
    { "checkpanic", checkpanic },
    { "newstate", newstate },
    { "newuserdata", newuserdata },
    { "num2int", num2int },
    { "pushuserdata", pushuserdata },
    { "querystr", string_query },
    { "querytab", table_query },
    { "ref", tref },
    { "resume", coresume },
    { "s2d", s2d },
    { "sethook", sethook },
    { "stacklevel", stacklevel },
    { "testC", testC },
    { "makeCfunc", makeCfunc },
    { "totalmem", mem_query },
    { "trick", settrick },
    { "udataval", udataval },
    { "unref", unref },
    { "upvalue", upvalues },
    { NULL, NULL }
};


static void checkfinalmem(void)
{
    lhat_assert(l_memcontrol.numblocks == 0);
    lhat_assert(l_memcontrol.total == 0);
}


int lhatB_opentests(lhat_State *L)
{
    void *ud;
    lhat_atpanic(L, &tpanic);
    atexit(checkfinalmem);
    lhat_assert(lhat_getallocf(L, &ud) == debug_realloc);
    lhat_assert(ud == cast(void *, &l_memcontrol));
    lhat_setallocf(L, lhat_getallocf(L, NULL), ud);
    lhatL_newlib(L, tests_funcs);
    return 1;
}

#endif