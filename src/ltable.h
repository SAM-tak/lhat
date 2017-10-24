//
// $Id: ltable.h,v 2.23 2016/12/22 13:08:50 roberto Exp $
// Lhat tables (hash)
// See Copyright Notice in lhat.h
//

#ifndef ltable_h
#define ltable_h

#include "lobject.h"


#define gnode(t,i)	(&(t)->node[i])
#define gval(n)		(&(n)->i_val)
#define gnext(n)	((n)->i_key.nk.next)


// 'const' to avoid wrong writings that can mess up field 'next'
#define gkey(n)		cast(const TValue*, (&(n)->i_key.tvk))

//
// writable version of 'gkey'; allows updates to individual fields,
// but not to the whole (which has incompatible type)
//
#define wgkey(n)		(&(n)->i_key.nk)

#define invalidateTMcache(t)	((t)->flags = 0)


// true when 't' is using 'dummynode' as its hash part
#define isdummy(t)		((t)->lastfree == NULL)


// allocated size for hash nodes
#define allocsizenode(t)	(isdummy(t) ? 0 : sizenode(t))


// returns the key, given the value of a table entry
#define keyfromval(v) \
  (gkey(cast(Node *, cast(char *, (v)) - offsetof(Node, i_val))))


LHATI_FUNC const TValue *lhatH_getint (Table *t, lhat_Integer key);
LHATI_FUNC void lhatH_setint (lhat_State *L, Table *t, lhat_Integer key,
                                                    TValue *value);
LHATI_FUNC const TValue *lhatH_getshortstr (Table *t, TString *key);
LHATI_FUNC const TValue *lhatH_getstr (Table *t, TString *key);
LHATI_FUNC const TValue *lhatH_get (Table *t, const TValue *key);
LHATI_FUNC TValue *lhatH_newkey (lhat_State *L, Table *t, const TValue *key);
LHATI_FUNC TValue *lhatH_set (lhat_State *L, Table *t, const TValue *key);
LHATI_FUNC Table *lhatH_new (lhat_State *L);
LHATI_FUNC void lhatH_resize (lhat_State *L, Table *t, unsigned int nasize,
                                                    unsigned int nhsize);
LHATI_FUNC void lhatH_resizearray (lhat_State *L, Table *t, unsigned int nasize);
LHATI_FUNC void lhatH_free (lhat_State *L, Table *t);
LHATI_FUNC int lhatH_next (lhat_State *L, Table *t, StkId key);
LHATI_FUNC int lhatH_getn (Table *t);


#if defined(LHAT_DEBUG)
LHATI_FUNC Node *lhatH_mainposition (const Table *t, const TValue *key);
LHATI_FUNC int lhatH_isdummy (const Table *t);
#endif


#endif
