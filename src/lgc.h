#ifndef lgc_h
#define lgc_h
//
// Garbage Collector
// See Copyright Notice in lhat.h
//

#include "lobject.h"
#include "lstate.h"

//
// Collectable objects may have one of three colors: white, which
// means the object is not marked; gray, which means the
// object is marked, but its references may be not marked; and
// black, which means that the object and all its references are marked.
// The main invariant of the garbage collector, while marking objects,
// is that a black object can never point to a white one. Moreover,
// any gray object must be in a "gray list" (gray, grayagain, weak,
// allweak, ephemeron) so that it can be visited again before finishing
// the collection cycle. These lists have no meaning when the invariant
// is not being enforced (e.g., sweep phase).
//



// how much to allocate before next GC step
#if !defined(GCSTEPSIZE)
// ~100 small strings
#define GCSTEPSIZE	(cast_int(100 * sizeof(TString)))
#endif


//
// Possible states of the Garbage Collector
//
#define GCSpropagate	0
#define GCSatomic	1
#define GCSswpallgc	2
#define GCSswpfinobj	3
#define GCSswptobefnz	4
#define GCSswpend	5
#define GCScallfin	6
#define GCSpause	7


#define issweepphase(g)  \
	(GCSswpallgc <= (g)->gcstate && (g)->gcstate <= GCSswpend)


//
// macro to tell when main invariant (white objects cannot point to black
// ones) must be kept. During a collection, the sweep
// phase may break the invariant, as objects turned white may point to
// still-black objects. The invariant is restored when sweep ends and
// all objects are white again.
//

#define keepinvariant(g)	((g)->gcstate <= GCSatomic)


//
// some useful bit tricks
//
#define resetbits(x,m)		((x) &= cast(lu_byte, ~(m)))
#define setbits(x,m)		((x) |= (m))
#define testbits(x,m)		((x) & (m))
#define bitmask(b)		(1<<(b))
#define bit2mask(b1,b2)		(bitmask(b1) | bitmask(b2))
#define l_setbit(x,b)		setbits(x, bitmask(b))
#define resetbit(x,b)		resetbits(x, bitmask(b))
#define testbit(x,b)		testbits(x, bitmask(b))


// Layout for bit use in 'marked' field:
#define WHITE0BIT	0  // object is white (type 0)
#define WHITE1BIT	1  // object is white (type 1)
#define BLACKBIT	2  // object is black
#define FINALIZEDBIT	3  // object has been marked for finalization
// bit 7 is currently used by tests (lhatL_checkmemory)

#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)


#define iswhite(x)      testbits((x)->marked, WHITEBITS)
#define isblack(x)      testbit((x)->marked, BLACKBIT)
// neither white nor black
#define isgray(x) (!testbits((x)->marked, WHITEBITS | bitmask(BLACKBIT)))

#define tofinalize(x)	testbit((x)->marked, FINALIZEDBIT)

#define otherwhite(g)	((g)->currentwhite ^ WHITEBITS)
#define isdeadm(ow,m)	(!(((m) ^ WHITEBITS) & (ow)))
#define isdead(g,v)	isdeadm(otherwhite(g), (v)->marked)

#define changewhite(x)	((x)->marked ^= WHITEBITS)
#define gray2black(x)	l_setbit((x)->marked, BLACKBIT)

#define lhatC_white(g)	cast(lu_byte, (g)->currentwhite & WHITEBITS)


//
// Does one step of collection when debt becomes positive. 'pre'/'pos'
// allows some adjustments to be done only when needed. macro
// 'condchangemem' is used only for heavy tests (forcing a full
// GC cycle on every opportunity)
//
#define lhatC_condGC(L,pre,pos) \
	{ if (G(L)->GCdebt > 0) { pre; lhatC_step(L); pos;}; \
	  condchangemem(L,pre,pos); }

// more often than not, 'pre'/'pos' are empty
#define lhatC_checkGC(L)		lhatC_condGC(L,(void)0,(void)0)


#define lhatC_barrier(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ?  \
	lhatC_barrier_(L,obj2gco(p),gcvalue(v)) : cast_void(0))

#define lhatC_barrierback(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ? \
	lhatC_barrierback_(L,p) : cast_void(0))

#define lhatC_objbarrier(L,p,o) (  \
	(isblack(p) && iswhite(o)) ? \
	lhatC_barrier_(L,obj2gco(p),obj2gco(o)) : cast_void(0))

#define lhatC_upvalbarrier(L,uv) ( \
	(iscollectable((uv)->v) && !upisopen(uv)) ? \
         lhatC_upvalbarrier_(L,uv) : cast_void(0))

LHATI_FUNC void lhatC_fix(lhat_State *L, GCObject *o);
LHATI_FUNC void lhatC_freeallobjects(lhat_State *L);
LHATI_FUNC void lhatC_step(lhat_State *L);
LHATI_FUNC void lhatC_runtilstate(lhat_State *L, int statesmask);
LHATI_FUNC void lhatC_fullgc(lhat_State *L, int isemergency);
LHATI_FUNC GCObject *lhatC_newobj(lhat_State *L, int tt, size_t sz);
LHATI_FUNC void lhatC_barrier_(lhat_State *L, GCObject *o, GCObject *v);
LHATI_FUNC void lhatC_barrierback_(lhat_State *L, Table *o);
LHATI_FUNC void lhatC_upvalbarrier_(lhat_State *L, Upvalue *uv);
LHATI_FUNC void lhatC_checkfinalizer(lhat_State *L, GCObject *o, Table *mt);
LHATI_FUNC void lhatC_upvdeccount(lhat_State *L, Upvalue *uv);


#endif
