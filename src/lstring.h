//
// $Id: lstring.h,v 1.61 2015/11/03 15:36:01 roberto Exp $
// String table (keep all strings handled by L^)
// See Copyright Notice in lhat.h
//

#ifndef lstring_h
#define lstring_h

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"


#define sizelstring(l)  (sizeof(union UTString) + ((l) + 1) * sizeof(char))

#define sizeludata(l)	(sizeof(union UUserData) + (l))
#define sizeudata(u)	sizeludata((u)->len)

#define lhatS_newliteral(L, s)	(lhatS_newlstr(L, "" s, \
                                 (sizeof(s)/sizeof(char))-1))


//
// test whether a string is a reserved word
//
#define isreserved(s)	((s)->tt == LHAT_TSHRSTR && (s)->extra > 0)


//
// equality for short strings, which are always internalized
//
#define eqshrstr(a,b)	check_exp((a)->tt == LHAT_TSHRSTR, (a) == (b))


LHATI_FUNC unsigned int lhatS_hash (const char *str, size_t l, unsigned int seed);
LHATI_FUNC unsigned int lhatS_hashlongstr (TString *ts);
LHATI_FUNC int lhatS_eqlngstr (TString *a, TString *b);
LHATI_FUNC void lhatS_resize (lhat_State *L, int newsize);
LHATI_FUNC void lhatS_clearcache (global_State *g);
LHATI_FUNC void lhatS_init (lhat_State *L);
LHATI_FUNC void lhatS_remove (lhat_State *L, TString *ts);
LHATI_FUNC UserData *lhatS_newudata (lhat_State *L, size_t s);
LHATI_FUNC TString *lhatS_newlstr (lhat_State *L, const char *str, size_t l);
LHATI_FUNC TString *lhatS_new (lhat_State *L, const char *str);
LHATI_FUNC TString *lhatS_createlngstrobj (lhat_State *L, size_t l);


#endif
