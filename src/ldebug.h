/*
** $Id: ldebug.h,v 2.14 2015/05/22 17:45:56 roberto Exp $
** Auxiliary functions from Debug Interface module
** See Copyright Notice in lhat.h
*/

#ifndef ldebug_h
#define ldebug_h


#include "lstate.h"


#define pcRel(pc, p)	(cast(int, (pc) - (p)->code) - 1)

#define getfuncline(f,pc)	(((f)->lineinfo) ? (f)->lineinfo[pc] : -1)

#define resethookcount(L)	(L->hookcount = L->basehookcount)


LHATI_FUNC l_noret lhatG_typeerror (lhat_State *L, const TValue *o,
                                                const char *opname);
LHATI_FUNC l_noret lhatG_concaterror (lhat_State *L, const TValue *p1,
                                                  const TValue *p2);
LHATI_FUNC l_noret lhatG_opinterror (lhat_State *L, const TValue *p1,
                                                 const TValue *p2,
                                                 const char *msg);
LHATI_FUNC l_noret lhatG_tointerror (lhat_State *L, const TValue *p1,
                                                 const TValue *p2);
LHATI_FUNC l_noret lhatG_ordererror (lhat_State *L, const TValue *p1,
                                                 const TValue *p2);
LHATI_FUNC l_noret lhatG_runerror (lhat_State *L, const char *fmt, ...);
LHATI_FUNC const char *lhatG_addinfo (lhat_State *L, const char *msg,
                                                  TString *src, int line);
LHATI_FUNC l_noret lhatG_errormsg (lhat_State *L);
LHATI_FUNC void lhatG_traceexec (lhat_State *L);


#endif
