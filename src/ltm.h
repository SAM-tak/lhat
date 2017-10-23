/*
** $Id: ltm.h,v 2.22 2016/02/26 19:20:15 roberto Exp $
** Tag methods
** See Copyright Notice in lhat.h
*/

#ifndef ltm_h
#define ltm_h


#include "lobject.h"


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER TM" and "ORDER OP"
*/
typedef enum {
  TM_INDEX,
  TM_NEWINDEX,
  TM_GC,
  TM_MODE,
  TM_LEN,
  TM_EQ,  /* last tag method with fast access */
  TM_ADD,
  TM_SUB,
  TM_MUL,
  TM_MOD,
  TM_POW,
  TM_DIV,
  TM_IDIV,
  TM_BAND,
  TM_BOR,
  TM_BXOR,
  TM_SHL,
  TM_SHR,
  TM_UNM,
  TM_BNOT,
  TM_LT,
  TM_LE,
  TM_CONCAT,
  TM_CALL,
  TM_N		/* number of elements in the enum */
} TMS;



#define gfasttm(g,et,e) ((et) == NULL ? NULL : \
  ((et)->flags & (1u<<(e))) ? NULL : lhatT_gettm(et, e, (g)->tmname[e]))

#define fasttm(l,et,e)	gfasttm(G(l), et, e)

#define ttypename(x)	lhatT_typenames_[(x) + 1]

LHATI_DDEC const char *const lhatT_typenames_[LHAT_TOTALTAGS];


LHATI_FUNC const char *lhatT_objtypename (lhat_State *L, const TValue *o);

LHATI_FUNC const TValue *lhatT_gettm (Table *events, TMS event, TString *ename);
LHATI_FUNC const TValue *lhatT_gettmbyobj (lhat_State *L, const TValue *o,
                                                       TMS event);
LHATI_FUNC void lhatT_init (lhat_State *L);

LHATI_FUNC void lhatT_callTM (lhat_State *L, const TValue *f, const TValue *p1,
                            const TValue *p2, TValue *p3, int hasres);
LHATI_FUNC int lhatT_callbinTM (lhat_State *L, const TValue *p1, const TValue *p2,
                              StkId res, TMS event);
LHATI_FUNC void lhatT_trybinTM (lhat_State *L, const TValue *p1, const TValue *p2,
                              StkId res, TMS event);
LHATI_FUNC int lhatT_callorderTM (lhat_State *L, const TValue *p1,
                                const TValue *p2, TMS event);



#endif
