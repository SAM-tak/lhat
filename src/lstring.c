//
// String table (keeps all strings handled by L^)
// See Copyright Notice in lhat.h
//

#define LHAT_CORE

#include "lprefix.h"

#include <string.h>

#include "lhat.h"

#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"


#define MEMERRMSG       "not enough memory"


//
// L^ will use at most ~(2^LHATI_HASHLIMIT) bytes from a string to
// compute its hash
//
#if !defined(LHATI_HASHLIMIT)
#define LHATI_HASHLIMIT		5
#endif


//
// equality for long strings
//
int lhatS_eqlngstr(TString *a, TString *b)
{
    size_t len = a->u.lnglen;
    lhat_assert(a->tt == LHAT_TLNGSTR && b->tt == LHAT_TLNGSTR);
    return (a == b) ||  // same instance or...
        ((len == b->u.lnglen) &&  // equal length and ...
        (memcmp(getstr(a), getstr(b), len) == 0));  // equal contents
}


unsigned int lhatS_hash(const char *str, size_t l, unsigned int seed)
{
    unsigned int h = seed ^ cast(unsigned int, l);
    size_t step = (l >> LHATI_HASHLIMIT) + 1;
    for(; l >= step; l -= step)
        h ^= ((h << 5) + (h >> 2) + cast_byte(str[l - 1]));
    return h;
}


unsigned int lhatS_hashlongstr(TString *ts)
{
    lhat_assert(ts->tt == LHAT_TLNGSTR);
    if(ts->extra == 0) {  // no hash?
        ts->hash = lhatS_hash(getstr(ts), ts->u.lnglen, ts->hash);
        ts->extra = 1;  // now it has its hash
    }
    return ts->hash;
}


//
// resizes the string table
//
void lhatS_resize(lhat_State *L, int newsize)
{
    StringTable *tb = &G(L)->strt;
    if(newsize > tb->size) {  // grow table if needed
        lhatM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
        for(int i = tb->size; i < newsize; i++)
            tb->hash[i] = NULL;
    }
    for(int i = 0; i < tb->size; i++) {  // rehash
        TString *p = tb->hash[i];
        tb->hash[i] = NULL;
        while(p) {  // for each node in the list
            TString *hnext = p->u.hnext;  // save next
            unsigned int h = lmod(p->hash, newsize);  // new position
            p->u.hnext = tb->hash[h];  // chain it
            tb->hash[h] = p;
            p = hnext;
        }
    }
    if(newsize < tb->size) {  // shrink table if needed
        // vanishing slice should be empty
        lhat_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
        lhatM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
    }
    tb->size = newsize;
}


//
// Clear API string cache. (Entries cannot be empty, so fill them with
// a non-collectable string.)
//
void lhatS_clearcache(GlobalState *g)
{
    for(int i = 0; i < STRCACHE_N; i++) {
        for(int j = 0; j < STRCACHE_M; j++) {
            if(iswhite(g->strcache[i][j]))  // will entry be collected?
                g->strcache[i][j] = g->memerrmsg;  // replace it with something fixed
        }
    }
}


//
// Initialize the string table and the string cache
//
void lhatS_init(lhat_State *L)
{
    GlobalState *g = G(L);
    lhatS_resize(L, MINSTRTABSIZE);  // initial size of string table
    // pre-create memory-error message
    g->memerrmsg = lhatS_newliteral(L, MEMERRMSG);
    lhatC_fix(L, obj2gco(g->memerrmsg));  // it should never be collected
    for(int i = 0; i < STRCACHE_N; i++)  // fill cache with valid strings
        for(int j = 0; j < STRCACHE_M; j++)
            g->strcache[i][j] = g->memerrmsg;
}



//
// creates a new string object
//
static TString *createstrobj(lhat_State *L, size_t l, int tag, unsigned int h)
{
    size_t totalsize;  // total size of TString object
    totalsize = sizelstring(l);
    GCObject *o = lhatC_newobj(L, tag, totalsize);
    TString *ts = gco2ts(o);
    ts->hash = h;
    ts->extra = 0;
    getstr(ts)[l] = '\0';  // ending 0
    return ts;
}


TString *lhatS_createlngstrobj(lhat_State *L, size_t l)
{
    TString *ts = createstrobj(L, l, LHAT_TLNGSTR, G(L)->seed);
    ts->u.lnglen = l;
    return ts;
}


void lhatS_remove(lhat_State *L, TString *ts)
{
    StringTable *tb = &G(L)->strt;
    TString **p = &tb->hash[lmod(ts->hash, tb->size)];
    while(*p != ts)  // find previous element
        p = &(*p)->u.hnext;
    *p = (*p)->u.hnext;  // remove element from its list
    tb->nuse--;
}


//
// checks whether short string exists and reuses it or creates a new one
//
static TString *internshrstr(lhat_State *L, const char *str, size_t l)
{
    GlobalState *g = G(L);
    unsigned int h = lhatS_hash(str, l, g->seed);
    TString **list = &g->strt.hash[lmod(h, g->strt.size)];
    lhat_assert(str != NULL);  // otherwise 'memcmp'/'memcpy' are undefined
    for(TString *ts = *list; ts != NULL; ts = ts->u.hnext) {
        if(l == ts->shrlen &&
            (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) {
            // found!
            if(isdead(g, ts))  // dead (but not collected yet)?
                changewhite(ts);  // resurrect it
            return ts;
        }
    }
    if(g->strt.nuse >= g->strt.size && g->strt.size <= MAX_INT / 2) {
        lhatS_resize(L, g->strt.size * 2);
        list = &g->strt.hash[lmod(h, g->strt.size)];  // recompute with new size
    }
    TString *ts = createstrobj(L, l, LHAT_TSHRSTR, h);
    memcpy(getstr(ts), str, l * sizeof(char));
    ts->shrlen = cast_byte(l);
    ts->u.hnext = *list;
    *list = ts;
    g->strt.nuse++;
    return ts;
}


//
// new string (with explicit length)
//
TString *lhatS_newlstr(lhat_State *L, const char *str, size_t l)
{
    if(l <= LHATI_MAXSHORTLEN)  // short string?
        return internshrstr(L, str, l);
    else {
        if(l >= (MAX_SIZE - sizeof(TString)) / sizeof(char))
            lhatM_toobig(L);
        TString *ts = lhatS_createlngstrobj(L, l);
        memcpy(getstr(ts), str, l * sizeof(char));
        return ts;
    }
}


//
// Create or reuse a zero-terminated string, first checking in the
// cache (using the string address as a key). The cache can contain
// only zero-terminated strings, so it is safe to use 'strcmp' to
// check hits.
//
TString *lhatS_new(lhat_State *L, const char *str)
{
    unsigned int i = point2uint(str) % STRCACHE_N;  // hash
    TString **p = G(L)->strcache[i];
    for(int j = 0; j < STRCACHE_M; j++) {
        if(strcmp(str, getstr(p[j])) == 0)  // hit?
            return p[j];  // that is it
    }
    // normal route
    for(int j = STRCACHE_M - 1; j > 0; j--)
        p[j] = p[j - 1];  // move out last element
                          // new element is first in the list
    p[0] = lhatS_newlstr(L, str, strlen(str));
    return p[0];
}


UserData *lhatS_newudata(lhat_State *L, size_t s)
{
    if(s > MAX_SIZE - sizeof(UserData))
        lhatM_toobig(L);
    GCObject *o = lhatC_newobj(L, LHAT_TUSERDATA, sizeludata(s));
    UserData *u = gco2u(o);
    u->len = s;
    u->metatable = NULL;
    setuservalue(L, u, lhatO_nilobject);
    return u;
}
