#ifndef lmetamethods_h
#define lmetamethods_h
//
// meta methods
// See Copyright Notice in lhat.h
//

#include "lobject.h"

//
// WARNING: if you change the order of this enumeration,
// grep "ORDER MM" and "ORDER OP"
//
typedef enum {
	MM_INDEX,
	MM_NEWINDEX,
	MM_GC,
	MM_MODE,
	MM_LEN,
	MM_EQ,  // last tag method with fast access
	MM_ADD,
	MM_SUB,
	MM_MUL,
	MM_MOD,
	MM_POW,
	MM_DIV,
	MM_IDIV,
	MM_BAND,
	MM_BOR,
	MM_BXOR,
	MM_SHL,
	MM_SHR,
	MM_UNM,
	MM_BNOT,
	MM_LT,
	MM_LE,
	MM_CONCAT,
	MM_CALL,
	MM_N		// number of elements in the enum
} MetaMethod;



#define gfastmm(g,et,e) ((et) == NULL ? NULL : \
  ((et)->flags & (1u<<(e))) ? NULL : lhatT_getMM(et, e, (g)->tmname[e]))

#define fastmm(l,et,e)	gfastmm(G(l), et, e)

LHATI_DDEC const char *const lhatT_typenames_[LHAT_TOTALTAGS];
inline const char *const ttypename(int x)
{
	return lhatT_typenames_[x + 1];
}

LHATI_FUNC const char *lhatT_objtypename(lhat_State *L, const TValue *o);

LHATI_FUNC const TValue *lhatT_getMM(Table *events, MetaMethod event, TString *ename);
LHATI_FUNC const TValue *lhatT_getMMByObj(lhat_State *L, const TValue *o, MetaMethod event);
LHATI_FUNC void lhatT_init(lhat_State *L);

LHATI_FUNC void lhatT_callMM(lhat_State *L, const TValue *f, const TValue *p1, const TValue *p2, TValue *p3, int hasres);
LHATI_FUNC int lhatT_callBinMM(lhat_State *L, const TValue *p1, const TValue *p2, StkId res, MetaMethod event);
LHATI_FUNC void lhatT_tryBinMM(lhat_State *L, const TValue *p1, const TValue *p2, StkId res, MetaMethod event);
LHATI_FUNC int lhatT_callOrderMM(lhat_State *L, const TValue *p1, const TValue *p2, MetaMethod event);

#endif // !lmetamethods_h