#ifndef lstate_h
#define lstate_h
//
// Global State
// See Copyright Notice in lhat.h
//

#include "lhat.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


//
// Some notes about garbage-collected objects: All objects in Lhat must
// be kept somehow accessible until being freed, so all objects always
// belong to one (and only one) of these lists, using field 'next' of
// the 'CommonHeader' for the link:
//
// 'allgc': all objects not marked for finalization;
// 'finobj': all objects marked for finalization;
// 'tobefnz': all objects ready to be finalized;
// 'fixedgc': all objects that are not to be collected (currently
// only small strings, such as reserved words).
//


// defined in ldo.c
struct lhat_longjmp;  


//
// Atomic type (relative to signals) to better ensure that 'lhat_sethook'
// is coroutine safe
//
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


// extra stack space to handle TM calls and some other extras
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LHAT_MINSTACK)


// kinds of Garbage Collection
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	// gc was forced by an allocation failure


typedef struct stringtable {
  TString **hash;
  int nuse;  // number of elements
  int size;
} stringtable;


//
// Information about a call.
// When a coroutine yields, 'func' is adjusted to pretend that the
// top function has only the yielded values in its stack; in that
// case, the actual 'func' value is saved in field 'extra'.
// When a function calls another with a continuation, 'extra' keeps
// the function index so that, in case of errors, the continuation
// function can be called with the correct top.
//
typedef struct CallInfo {
  StkId func;  // function index in the stack
  StkId	top;  // top for this function
  struct CallInfo *previous, *next;  // dynamic call link
  union {
    struct {  // only for Lhat functions
      StkId base;  // base for this function
      const Instruction *savedpc;
    } l;
    struct {  // only for C functions
      lhat_KFunction k;  // continuation in case of yields
      ptrdiff_t old_errfunc;
      lhat_KContext ctx;  // context info. in case of yields
    } c;
  } u;
  ptrdiff_t extra;
  short nresults;  // expected number of results from this function
  unsigned short callstatus;
} CallInfo;


//
// Bits in CallInfo status
//
#define CIST_OAH	(1<<0)	// original value of 'allowhook'
#define CIST_LHAT	(1<<1)	// call is running a Lhat function
#define CIST_HOOKED	(1<<2)	// call is running a debug hook
#define CIST_FRESH	(1<<3)	// call is running on a fresh invocation of lhatV_execute
#define CIST_YPCALL	(1<<4)	// call is a yieldable protected call
#define CIST_TAIL	(1<<5)	// call was tail called
#define CIST_HOOKYIELD	(1<<6)	// last hook called yielded
#define CIST_LEQ	(1<<7)  // using __lt for __le
#define CIST_FIN	(1<<8)  // call is running a finalizer

#define isLhat(ci)	((ci)->callstatus & CIST_LHAT)

// assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


//
// 'global state', shared by all coroutines of this state
//
typedef struct global_State {
  lhat_Alloc frealloc;  // function to reallocate memory
  void *ud;         // auxiliary data to 'frealloc'
  l_mem totalbytes;  // number of bytes currently allocated - GCdebt
  l_mem GCdebt;  // bytes allocated not yet compensated by the collector
  lu_mem GCmemtrav;  // memory traversed by the GC
  lu_mem GCestimate;  // an estimate of the non-garbage memory in use
  stringtable strt;  // hash table for strings
  TValue l_registry;
  unsigned int seed;  // randomized seed for hashes
  lu_byte currentwhite;
  lu_byte gcstate;  // state of garbage collector
  lu_byte gckind;  // kind of GC running
  lu_byte gcrunning;  // true if GC is running
  GCObject *allgc;  // list of all collectable objects
  GCObject **sweepgc;  // current position of sweep in list
  GCObject *finobj;  // list of collectable objects with finalizers
  GCObject *gray;  // list of gray objects
  GCObject *grayagain;  // list of objects to be traversed atomically
  GCObject *weak;  // list of tables with weak values
  GCObject *ephemeron;  // list of ephemeron tables (weak keys)
  GCObject *allweak;  // list of all-weak tables
  GCObject *tobefnz;  // list of userdata to be GC
  GCObject *fixedgc;  // list of objects not to be collected
  struct lhat_State *twups;  // list of coroutines with open upvalues
  unsigned int gcfinnum;  // number of finalizers to call in each GC step
  int gcpause;  // size of pause between successive GCs
  int gcstepmul;  // GC 'granularity'
  lhat_CFunction panic;  // to be called in unprotected errors
  struct lhat_State *maincoroutine;
  const lhat_Number *version;  // pointer to version number
  TString *memerrmsg;  // memory-error message
  TString *tmname[TM_N];  // array with tag-method names
  struct Table *mt[LHAT_NUMTAGS];  // metatables for basic types
  TString *strcache[STRCACHE_N][STRCACHE_M];  // cache for strings in API
} global_State;


//
// 'per coroutine' state
//
struct lhat_State {
  CommonHeader;
  unsigned short nci;  // number of items in 'ci' list
  lu_byte status;
  StkId top;  // first free slot in the stack
  global_State *l_G;
  CallInfo *ci;  // call info for current function
  const Instruction *oldpc;  // last pc traced
  StkId stack_last;  // last free slot in the stack
  StkId stack;  // stack base
  Upvalue *openupval;  // list of open upvalues in this stack
  GCObject *gclist;
  struct lhat_State *twups;  // list of coroutines with open upvalues
  struct lhat_longjmp *errorJmp;  // current error recover point
  CallInfo base_ci;  // CallInfo for first level (C calling Lhat)
  volatile lhat_Hook hook;
  ptrdiff_t errfunc;  // current error handling function (stack index)
  int stacksize;
  int basehookcount;
  int hookcount;
  unsigned short nny;  // number of non-yieldable calls in stack
  unsigned short nCcalls;  // number of nested C calls
  l_signalT hookmask;
  lu_byte allowhook;
};


#define G(L)	(L->l_G)


//
// Union of all collectable objects (only for conversions)
//
union GCUnion {
  GCObject gc;  // common header
  struct TString ts;
  struct Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct lhat_State th;  // coroutine
};


#define cast_u(o)	cast(union GCUnion *, (o))

// macros to convert a GCObject into a specific value
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LHAT_TSTRING, &((cast_u(o))->ts))
#define gco2u(o)  check_exp((o)->tt == LHAT_TUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LHAT_TLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LHAT_TCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LHAT_TFUNCTION, &((cast_u(o))->cl))
#define gco2t(o)  check_exp((o)->tt == LHAT_TTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LHAT_TPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LHAT_TCOROUTINE, &((cast_u(o))->th))


// macro to convert a Lhat object into a GCObject
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LHAT_TDEADKEY, (&(cast_u(v)->gc)))


// actual number of total bytes allocated
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LHATI_FUNC void lhatE_setdebt (global_State *g, l_mem debt);
LHATI_FUNC void lhatE_freecoroutine (lhat_State *L, lhat_State *L1);
LHATI_FUNC CallInfo *lhatE_extendCI (lhat_State *L);
LHATI_FUNC void lhatE_freeCI (lhat_State *L);
LHATI_FUNC void lhatE_shrinkCI (lhat_State *L);

#endif