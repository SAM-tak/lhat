//
// Standard library for string operations and pattern-matching
// See Copyright Notice in lhat.h
//

#define LHAT_LIB

#include "lprefix.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"

//
// maximum number of captures that a pattern can do during
// pattern-matching. This limit is arbitrary, but must fit in
// an unsigned char.
//
#if !defined(LHAT_MAXCAPTURES)
# define LHAT_MAXCAPTURES		32
#endif


// macro to 'unsign' a character
#define uchar(c)	((unsigned char)(c))


//
// Some sizes are better limited to fit in 'int', but must also fit in
// 'size_t'. (We assume that 'lhat_Integer' cannot be smaller than 'int'.)
//
#define MAX_SIZET	((size_t)(~(size_t)0))

#define MAXSIZE  \
	(sizeof(size_t) < sizeof(int) ? MAX_SIZET : (size_t)(INT_MAX))




static int str_len(lhat_State *L)
{
    size_t l;
    lhatL_checklstring(L, 1, &l);
    lhat_pushinteger(L, (lhat_Integer)l);
    return 1;
}


// translate a relative string position: negative means back from end
static lhat_Integer posrelat(lhat_Integer pos, size_t len)
{
    if(pos >= 0) return pos;
    else if(0u - (size_t)pos > len) return 0;
    else return (lhat_Integer)len + pos + 1;
}


static int str_sub(lhat_State *L)
{
    size_t l;
    const char *s = lhatL_checklstring(L, 1, &l);
    lhat_Integer start = posrelat(lhatL_checkinteger(L, 2), l);
    lhat_Integer end = posrelat(lhatL_optinteger(L, 3, -1), l);
    if(start < 1) start = 1;
    if(end >(lhat_Integer)l) end = l;
    if(start <= end)
        lhat_pushlstring(L, s + start - 1, (size_t)(end - start) + 1);
    else lhat_pushliteral(L, "");
    return 1;
}


static int str_reverse(lhat_State *L)
{
    size_t l, i;
    lhatL_Buffer b;
    const char *s = lhatL_checklstring(L, 1, &l);
    char *p = lhatL_buffinitsize(&b, l, L);
    for(i = 0; i < l; i++)
        p[i] = s[l - i - 1];
    lhatL_pushresultsize(&b, l);
    return 1;
}


static int str_lower(lhat_State *L)
{
    size_t l;
    const char *s = lhatL_checklstring(L, 1, &l);
    lhatL_Buffer b;
    char *p = lhatL_buffinitsize(&b, l, L);
    for(size_t i = 0; i < l; i++)
        p[i] = tolower(uchar(s[i]));
    lhatL_pushresultsize(&b, l);
    return 1;
}


static int str_upper(lhat_State *L)
{
    size_t l;
    const char *s = lhatL_checklstring(L, 1, &l);
    lhatL_Buffer b;
    char *p = lhatL_buffinitsize(&b, l, L);
    for(size_t i = 0; i < l; i++)
        p[i] = toupper(uchar(s[i]));
    lhatL_pushresultsize(&b, l);
    return 1;
}


static int str_rep(lhat_State *L)
{
    size_t l, lsep;
    const char *s = lhatL_checklstring(L, 1, &l);
    lhat_Integer n = lhatL_checkinteger(L, 2);
    const char *sep = lhatL_optlstring(L, 3, "", &lsep);
    if(n <= 0) lhat_pushliteral(L, "");
    else if(l + lsep < l || l + lsep > MAXSIZE / n)  // may overflow?
        return lhatL_error(L, "resulting string too large");
    else {
        size_t totallen = (size_t)n * l + (size_t)(n - 1) * lsep;
        lhatL_Buffer b;
        char *p = lhatL_buffinitsize(&b, totallen, L);
        while(n-- > 1) {  // first n-1 copies (followed by separator)
            memcpy(p, s, l * sizeof(char)); p += l;
            if(lsep > 0) {  // empty 'memcpy' is not that cheap
                memcpy(p, sep, lsep * sizeof(char));
                p += lsep;
            }
        }
        memcpy(p, s, l * sizeof(char));  // last copy (not followed by separator)
        lhatL_pushresultsize(&b, totallen);
    }
    return 1;
}


static int str_byte(lhat_State *L)
{
    size_t l;
    const char *s = lhatL_checklstring(L, 1, &l);
    lhat_Integer posi = posrelat(lhatL_optinteger(L, 2, 1), l);
    lhat_Integer pose = posrelat(lhatL_optinteger(L, 3, posi), l);
    int n, i;
    if(posi < 1) posi = 1;
    if(pose >(lhat_Integer)l) pose = l;
    if(posi > pose) return 0;  // empty interval; return no values
    if(pose - posi >= INT_MAX)  // arithmetic overflow?
        return lhatL_error(L, "string slice too long");
    n = (int)(pose - posi) + 1;
    lhatL_checkstack(L, n, "string slice too long");
    for(i = 0; i<n; i++)
        lhat_pushinteger(L, uchar(s[posi + i - 1]));
    return n;
}


static int str_char(lhat_State *L)
{
    int n = lhat_gettop(L);  // number of arguments
    int i;
    lhatL_Buffer b;
    char *p = lhatL_buffinitsize(&b, n, L);
    for(i = 1; i <= n; i++) {
        lhat_Integer c = lhatL_checkinteger(L, i);
        lhatL_argcheck(L, uchar(c) == c, i, "value out of range");
        p[i - 1] = uchar(c);
    }
    lhatL_pushresultsize(&b, n);
    return 1;
}


static int writer(lhat_State *L, const void *b, size_t size, void *B)
{
    (void)L;
    lhatL_addlstring((lhatL_Buffer *)B, (const char *)b, size);
    return 0;
}


static int str_dump(lhat_State *L)
{
    lhatL_Buffer b;
    int strip = lhat_toboolean(L, 2);
    lhatL_checktype(L, 1, LHAT_TFUNCTION);
    lhat_settop(L, 1);
    lhatL_buffinit(&b, L);
    if(lhat_dump(L, writer, &b, strip) != 0)
        return lhatL_error(L, "unable to dump given function");
    lhatL_pushresult(&b);
    return 1;
}



//
// {======================================================
// PATTERN MATCHING
// =======================================================
//


#define CAP_UNFINISHED	(-1)
#define CAP_POSITION	(-2)


typedef struct MatchState {
    const char *src_init;  // init of source string
    const char *src_end;  // end ('\0') of source string
    const char *p_end;  // end ('\0') of pattern
    lhat_State *L;
    int matchdepth;  // control for recursive depth (to avoid C stack overflow)
    unsigned char level;  // total number of captures (finished or unfinished)
    struct {
        const char *init;
        ptrdiff_t len;
    } capture[LHAT_MAXCAPTURES];
} MatchState;


// recursive function
static const char *match(MatchState *ms, const char *s, const char *p);


// maximum recursion depth for 'match'
#if !defined(MAXCCALLS)
#define MAXCCALLS	200
#endif


#define L_ESC		'%'
#define SPECIALS	"^$*+?.([%-"


static int check_capture(MatchState *ms, int l)
{
    l -= '1';
    if(l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED)
        return lhatL_error(ms->L, "invalid capture index %%%d", l + 1);
    return l;
}


static int capture_to_close(MatchState *ms)
{
    int level = ms->level;
    for(level--; level >= 0; level--)
        if(ms->capture[level].len == CAP_UNFINISHED) return level;
    return lhatL_error(ms->L, "invalid pattern capture");
}


static const char *classend(MatchState *ms, const char *p)
{
    switch(*p++) {
    case L_ESC: {
        if(p == ms->p_end)
            lhatL_error(ms->L, "malformed pattern (ends with '%%')");
        return p + 1;
    }
    case '[': {
        if(*p == '^') p++;
        do {  // look for a ']'
            if(p == ms->p_end)
                lhatL_error(ms->L, "malformed pattern (missing ']')");
            if(*(p++) == L_ESC && p < ms->p_end)
                p++;  // skip escapes (e.g. '%]')
        } while(*p != ']');
        return p + 1;
    }
    default: {
        return p;
    }
    }
}


static int match_class(int c, int cl)
{
    int res;
    switch(tolower(cl)) {
    case 'a': res = isalpha(c); break;
    case 'c': res = iscntrl(c); break;
    case 'd': res = isdigit(c); break;
    case 'g': res = isgraph(c); break;
    case 'l': res = islower(c); break;
    case 'p': res = ispunct(c); break;
    case 's': res = isspace(c); break;
    case 'u': res = isupper(c); break;
    case 'w': res = isalnum(c); break;
    case 'x': res = isxdigit(c); break;
    case 'z': res = (c == 0); break;  // deprecated option
    default: return (cl == c);
    }
    return (islower(cl) ? res : !res);
}


static int matchbracketclass(int c, const char *p, const char *ec)
{
    int sig = 1;
    if(*(p + 1) == '^') {
        sig = 0;
        p++;  // skip the '^'
    }
    while(++p < ec) {
        if(*p == L_ESC) {
            p++;
            if(match_class(c, uchar(*p)))
                return sig;
        }
        else if((*(p + 1) == '-') && (p + 2 < ec)) {
            p += 2;
            if(uchar(*(p - 2)) <= c && c <= uchar(*p))
                return sig;
        }
        else if(uchar(*p) == c) return sig;
    }
    return !sig;
}


static int singlematch(MatchState *ms, const char *s, const char *p, const char *ep)
{
    if(s >= ms->src_end)
        return 0;
    else {
        int c = uchar(*s);
        switch(*p) {
        case '.': return 1;  // matches any char
        case L_ESC: return match_class(c, uchar(*(p + 1)));
        case '[': return matchbracketclass(c, p, ep - 1);
        default:  return (uchar(*p) == c);
        }
    }
}


static const char *matchbalance(MatchState *ms, const char *s, const char *p)
{
    if(p >= ms->p_end - 1)
        lhatL_error(ms->L, "malformed pattern (missing arguments to '%%b')");
    if(*s != *p) return NULL;
    else {
        int b = *p;
        int e = *(p + 1);
        int cont = 1;
        while(++s < ms->src_end) {
            if(*s == e) {
                if(--cont == 0) return s + 1;
            }
            else if(*s == b) cont++;
        }
    }
    return NULL;  // string ends out of balance
}


static const char *max_expand(MatchState *ms, const char *s, const char *p, const char *ep)
{
    ptrdiff_t i = 0;  // counts maximum expand for item
    while(singlematch(ms, s + i, p, ep))
        i++;
    // keeps trying to match with the maximum repetitions
    while(i >= 0) {
        const char *res = match(ms, (s + i), ep + 1);
        if(res) return res;
        i--;  // else didn't match; reduce 1 repetition to try again
    }
    return NULL;
}


static const char *min_expand(MatchState *ms, const char *s, const char *p, const char *ep)
{
    for(;;) {
        const char *res = match(ms, s, ep + 1);
        if(res != NULL)
            return res;
        else if(singlematch(ms, s, p, ep))
            s++;  // try with one more repetition
        else return NULL;
    }
}


static const char *start_capture(MatchState *ms, const char *s, const char *p, int what)
{
    const char *res;
    int level = ms->level;
    if(level >= LHAT_MAXCAPTURES) lhatL_error(ms->L, "too many captures");
    ms->capture[level].init = s;
    ms->capture[level].len = what;
    ms->level = level + 1;
    if((res = match(ms, s, p)) == NULL)  // match failed?
        ms->level--;  // undo capture
    return res;
}


static const char *end_capture(MatchState *ms, const char *s, const char *p)
{
    int l = capture_to_close(ms);
    const char *res;
    ms->capture[l].len = s - ms->capture[l].init;  // close capture
    if((res = match(ms, s, p)) == NULL)  // match failed?
        ms->capture[l].len = CAP_UNFINISHED;  // undo capture
    return res;
}


static const char *match_capture(MatchState *ms, const char *s, int l)
{
    size_t len;
    l = check_capture(ms, l);
    len = ms->capture[l].len;
    if((size_t)(ms->src_end - s) >= len &&
        memcmp(ms->capture[l].init, s, len) == 0)
        return s + len;
    else return NULL;
}


static const char *match(MatchState *ms, const char *s, const char *p)
{
    if(ms->matchdepth-- == 0)
        lhatL_error(ms->L, "pattern too complex");
init: // using goto's to optimize tail recursion
    if(p != ms->p_end) {  // end of pattern?
        switch(*p) {
        case '(': {  // start capture
            if(*(p + 1) == ')')  // position capture?
                s = start_capture(ms, s, p + 2, CAP_POSITION);
            else
                s = start_capture(ms, s, p + 1, CAP_UNFINISHED);
            break;
        }
        case ')': {  // end capture
            s = end_capture(ms, s, p + 1);
            break;
        }
        case '$': {
            if((p + 1) != ms->p_end)  // is the '$' the last char in pattern?
                goto dflt;  // no; go to default
            s = (s == ms->src_end) ? s : NULL;  // check end of string
            break;
        }
        case L_ESC: {  // escaped sequences not in the format class[*+?-]?
            switch(*(p + 1)) {
            case 'b': {  // balanced string?
                s = matchbalance(ms, s, p + 2);
                if(s != NULL) {
                    p += 4; goto init;  // return match(ms, s, p + 4);
                }  // else fail (s == NULL)
                break;
            }
            case 'f': {  // frontier?
                const char *ep; char previous;
                p += 2;
                if(*p != '[')
                    lhatL_error(ms->L, "missing '[' after '%%f' in pattern");
                ep = classend(ms, p);  // points to what is next
                previous = (s == ms->src_init) ? '\0' : *(s - 1);
                if(!matchbracketclass(uchar(previous), p, ep - 1) &&
                    matchbracketclass(uchar(*s), p, ep - 1)) {
                    p = ep; goto init;  // return match(ms, s, ep);
                }
                s = NULL;  // match failed
                break;
            }
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7':
            case '8': case '9': {  // capture results (%0-%9)?
                s = match_capture(ms, s, uchar(*(p + 1)));
                if(s != NULL) {
                    p += 2; goto init;  // return match(ms, s, p + 2)
                }
                break;
            }
            default: goto dflt;
            }
            break;
        }
        default: dflt: {  // pattern class plus optional suffix
            const char *ep = classend(ms, p);  // points to optional suffix
                                               // does not match at least once?
            if(!singlematch(ms, s, p, ep)) {
                if(*ep == '*' || *ep == '?' || *ep == '-') {  // accept empty?
                    p = ep + 1; goto init;  // return match(ms, s, ep + 1);
                }
                else  // '+' or no suffix
                    s = NULL;  // fail
            }
            else {  // matched once
                switch(*ep) {  // handle optional suffix
                case '?': {  // optional
                    const char *res;
                    if((res = match(ms, s + 1, ep + 1)) != NULL)
                        s = res;
                    else {
                        p = ep + 1; goto init;  // else return match(ms, s, ep + 1);
                    }
                    break;
                }
                case '+':  // 1 or more repetitions
                    s++;  // 1 match already done
                          // FALLTHROUGH
                case '*':  // 0 or more repetitions
                    s = max_expand(ms, s, p, ep);
                    break;
                case '-':  // 0 or more repetitions (minimum)
                    s = min_expand(ms, s, p, ep);
                    break;
                default:  // no suffix
                    s++; p = ep; goto init;  // return match(ms, s + 1, ep);
                }
            }
            break;
        }
        }
    }
    ms->matchdepth++;
    return s;
}



static const char *lmemfind(const char *s1, size_t l1, const char *s2, size_t l2)
{
    if(l2 == 0) return s1;  // empty strings are everywhere
    else if(l2 > l1) return NULL;  // avoids a negative 'l1'
    else {
        const char *init;  // to search for a '*s2' inside 's1'
        l2--;  // 1st char will be checked by 'memchr'
        l1 = l1 - l2;  // 's2' cannot be found after that
        while(l1 > 0 && (init = (const char *)memchr(s1, *s2, l1)) != NULL) {
            init++;   // 1st char is already checked
            if(memcmp(init, s2 + 1, l2) == 0)
                return init - 1;
            else {  // correct 'l1' and 's1' to try again
                l1 -= init - s1;
                s1 = init;
            }
        }
        return NULL;  // not found
    }
}


static void push_onecapture(MatchState *ms, int i, const char *s, const char *e)
{
    if(i >= ms->level) {
        if(i == 0)  // ms->level == 0, too
            lhat_pushlstring(ms->L, s, e - s);  // add whole match
        else
            lhatL_error(ms->L, "invalid capture index %%%d", i + 1);
    }
    else {
        ptrdiff_t l = ms->capture[i].len;
        if(l == CAP_UNFINISHED) lhatL_error(ms->L, "unfinished capture");
        if(l == CAP_POSITION)
            lhat_pushinteger(ms->L, (ms->capture[i].init - ms->src_init) + 1);
        else
            lhat_pushlstring(ms->L, ms->capture[i].init, l);
    }
}


static int push_captures(MatchState *ms, const char *s, const char *e)
{
    int i;
    int nlevels = (ms->level == 0 && s) ? 1 : ms->level;
    lhatL_checkstack(ms->L, nlevels, "too many captures");
    for(i = 0; i < nlevels; i++)
        push_onecapture(ms, i, s, e);
    return nlevels;  // number of strings pushed
}


// check whether pattern has no special characters
static int nospecials(const char *p, size_t l)
{
    size_t upto = 0;
    do {
        if(strpbrk(p + upto, SPECIALS))
            return 0;  // pattern has a special character
        upto += strlen(p + upto) + 1;  // may have more after \0
    } while(upto <= l);
    return 1;  // no special chars found
}


static void prepstate(MatchState *ms, lhat_State *L,
    const char *s, size_t ls, const char *p, size_t lp)
{
    ms->L = L;
    ms->matchdepth = MAXCCALLS;
    ms->src_init = s;
    ms->src_end = s + ls;
    ms->p_end = p + lp;
}


static void reprepstate(MatchState *ms)
{
    ms->level = 0;
    lhat_assert(ms->matchdepth == MAXCCALLS);
}


static int str_find_aux(lhat_State *L, int find)
{
    size_t ls, lp;
    const char *s = lhatL_checklstring(L, 1, &ls);
    const char *p = lhatL_checklstring(L, 2, &lp);
    lhat_Integer init = posrelat(lhatL_optinteger(L, 3, 1), ls);
    if(init < 1) init = 1;
    else if(init >(lhat_Integer)ls + 1) {  // start after string's end?
        lhat_pushnil(L);  // cannot find anything
        return 1;
    }
    // explicit request or no special characters?
    if(find && (lhat_toboolean(L, 4) || nospecials(p, lp))) {
        // do a plain search
        const char *s2 = lmemfind(s + init - 1, ls - (size_t)init + 1, p, lp);
        if(s2) {
            lhat_pushinteger(L, (s2 - s) + 1);
            lhat_pushinteger(L, (s2 - s) + lp);
            return 2;
        }
    }
    else {
        MatchState ms;
        const char *s1 = s + init - 1;
        int anchor = (*p == '^');
        if(anchor) {
            p++; lp--;  // skip anchor character
        }
        prepstate(&ms, L, s, ls, p, lp);
        do {
            const char *res;
            reprepstate(&ms);
            if((res = match(&ms, s1, p)) != NULL) {
                if(find) {
                    lhat_pushinteger(L, (s1 - s) + 1);  // start
                    lhat_pushinteger(L, res - s);   // end
                    return push_captures(&ms, NULL, 0) + 2;
                }
                else
                    return push_captures(&ms, s1, res);
            }
        } while(s1++ < ms.src_end && !anchor);
    }
    lhat_pushnil(L);  // not found
    return 1;
}


static int str_find(lhat_State *L)
{
    return str_find_aux(L, 1);
}


static int str_match(lhat_State *L)
{
    return str_find_aux(L, 0);
}


// state for 'gmatch'
typedef struct GMatchState {
    const char *src;  // current position
    const char *p;  // pattern
    const char *lastmatch;  // end of last match
    MatchState ms;  // match state
} GMatchState;


static int gmatch_aux(lhat_State *L)
{
    GMatchState *gm = (GMatchState *)lhat_touserdata(L, lhat_upvalueindex(3));
    const char *src;
    gm->ms.L = L;
    for(src = gm->src; src <= gm->ms.src_end; src++) {
        const char *e;
        reprepstate(&gm->ms);
        if((e = match(&gm->ms, src, gm->p)) != NULL && e != gm->lastmatch) {
            gm->src = gm->lastmatch = e;
            return push_captures(&gm->ms, src, e);
        }
    }
    return 0;  // not found
}


static int gmatch(lhat_State *L)
{
    size_t ls, lp;
    const char *s = lhatL_checklstring(L, 1, &ls);
    const char *p = lhatL_checklstring(L, 2, &lp);
    GMatchState *gm;
    lhat_settop(L, 2);  // keep them on closure to avoid being collected
    gm = (GMatchState *)lhat_newuserdata(L, sizeof(GMatchState));
    prepstate(&gm->ms, L, s, ls, p, lp);
    gm->src = s; gm->p = p; gm->lastmatch = NULL;
    lhat_pushcclosure(L, gmatch_aux, 3);
    return 1;
}


static void add_s(MatchState *ms, lhatL_Buffer *b, const char *s, const char *e)
{
    size_t l, i;
    lhat_State *L = ms->L;
    const char *news = lhat_tolstring(L, 3, &l);
    for(i = 0; i < l; i++) {
        if(news[i] != L_ESC)
            lhatL_addchar(b, news[i]);
        else {
            i++;  // skip ESC
            if(!isdigit(uchar(news[i]))) {
                if(news[i] != L_ESC)
                    lhatL_error(L, "invalid use of '%c' in replacement string", L_ESC);
                lhatL_addchar(b, news[i]);
            }
            else if(news[i] == '0')
                lhatL_addlstring(b, s, e - s);
            else {
                push_onecapture(ms, news[i] - '1', s, e);
                lhatL_tolstring(L, -1, NULL);  // if number, convert it to string
                lhat_remove(L, -2);  // remove original value
                lhatL_addvalue(b);  // add capture to accumulated result
            }
        }
    }
}


static void add_value(MatchState *ms, lhatL_Buffer *b, const char *s, const char *e, int tr)
{
    lhat_State *L = ms->L;
    switch(tr) {
    case LHAT_TFUNCTION: {
        int n;
        lhat_pushvalue(L, 3);
        n = push_captures(ms, s, e);
        lhat_call(L, n, 1);
        break;
    }
    case LHAT_TTABLE: {
        push_onecapture(ms, 0, s, e);
        lhat_gettable(L, 3);
        break;
    }
    default: {  // LHAT_TNUMBER or LHAT_TSTRING
        add_s(ms, b, s, e);
        return;
    }
    }
    if(!lhat_toboolean(L, -1)) {  // nil or false?
        lhat_pop(L, 1);
        lhat_pushlstring(L, s, e - s);  // keep original text
    }
    else if(!lhat_isstring(L, -1))
        lhatL_error(L, "invalid replacement value (a %s)", lhatL_typename(L, -1));
    lhatL_addvalue(b);  // add result to accumulator
}


static int str_gsub(lhat_State *L)
{
    size_t srcl, lp;
    const char *src = lhatL_checklstring(L, 1, &srcl);  // subject
    const char *p = lhatL_checklstring(L, 2, &lp);  // pattern
    const char *lastmatch = NULL;  // end of last match
    int tr = lhat_type(L, 3);  // replacement type
    lhat_Integer max_s = lhatL_optinteger(L, 4, srcl + 1);  // max replacements
    int anchor = (*p == '^');
    lhat_Integer n = 0;  // replacement count
    MatchState ms;
    lhatL_Buffer b;
    lhatL_argcheck(L, tr == LHAT_TNUMBER || tr == LHAT_TSTRING ||
        tr == LHAT_TFUNCTION || tr == LHAT_TTABLE, 3,
        "string/function/table expected");
    lhatL_buffinit(&b, L);
    if(anchor) {
        p++; lp--;  // skip anchor character
    }
    prepstate(&ms, L, src, srcl, p, lp);
    while(n < max_s) {
        const char *e;
        reprepstate(&ms);  // (re)prepare state for new match
        if((e = match(&ms, src, p)) != NULL && e != lastmatch) {  // match?
            n++;
            add_value(&ms, &b, src, e, tr);  // add replacement to buffer
            src = lastmatch = e;
        }
        else if(src < ms.src_end)  // otherwise, skip one character
            lhatL_addchar(&b, *src++);
        else break;  // end of subject
        if(anchor) break;
    }
    lhatL_addlstring(&b, src, ms.src_end - src);
    lhatL_pushresult(&b);
    lhat_pushinteger(L, n);  // number of substitutions
    return 2;
}

// }======================================================



//
// {======================================================
// STRING FORMAT
// =======================================================
//

#if !defined(lhat_number2strx)	// {

//
// Hexadecimal floating-point formatter
//

#include <math.h>

#define SIZELENMOD	(sizeof(LHAT_NUMBER_FRMLEN)/sizeof(char))


//
// Number of bits that goes into the first digit. It can be any value
// between 1 and 4; the following definition tries to align the number
// to nibble boundaries by making what is left after that first digit a
// multiple of 4.
//
#define L_NBFD		((l_mathlim(MANT_DIG) - 1)%4 + 1)


//
// Add integer part of 'x' to buffer and return new 'x'
//
static lhat_Number adddigit(char *buff, int n, lhat_Number x)
{
    lhat_Number dd = l_mathop(floor)(x);  // get integer part from 'x'
    int d = (int)dd;
    buff[n] = (d < 10 ? d + '0' : d - 10 + 'a');  // add to buffer
    return x - dd;  // return what is left
}


static int num2straux(char *buff, int sz, lhat_Number x)
{
    // if 'inf' or 'NaN', format it like '%g'
    if(x != x || x == (lhat_Number)HUGE_VAL || x == -(lhat_Number)HUGE_VAL)
        return l_sprintf(buff, sz, LHAT_NUMBER_FMT, (LHATI_UACNUMBER)x);
    else if(x == 0) {  // can be -0...
                       // create "0" or "-0" followed by exponent
        return l_sprintf(buff, sz, LHAT_NUMBER_FMT "x0p+0", (LHATI_UACNUMBER)x);
    }
    else {
        int e;
        lhat_Number m = l_mathop(frexp)(x, &e);  // 'x' fraction and exponent
        int n = 0;  // character count
        if(m < 0) {  // is number negative?
            buff[n++] = '-';  // add signal
            m = -m;  // make it positive
        }
        buff[n++] = '0'; buff[n++] = 'x';  // add "0x"
        m = adddigit(buff, n++, m * (1 << L_NBFD));  // add first digit
        e -= L_NBFD;  // this digit goes before the radix point
        if(m > 0) {  // more digits?
            buff[n++] = lhat_getlocaledecpoint();  // add radix point
            do {  // add as many digits as needed
                m = adddigit(buff, n++, m * 16);
            } while(m > 0);
        }
        n += l_sprintf(buff + n, sz - n, "p%+d", e);  // add exponent
        lhat_assert(n < sz);
        return n;
    }
}


static int lhat_number2strx(lhat_State *L, char *buff, int sz,
    const char *fmt, lhat_Number x)
{
    int n = num2straux(buff, sz, x);
    if(fmt[SIZELENMOD] == 'A') {
        int i;
        for(i = 0; i < n; i++)
            buff[i] = toupper(uchar(buff[i]));
    }
    else if(fmt[SIZELENMOD] != 'a')
        lhatL_error(L, "modifiers for format '%%a'/'%%A' not implemented");
    return n;
}

#endif				// }


//
// Maximum size of each formatted item. This maximum size is produced
// by format('%.99f', -maxfloat), and is equal to 99 + 3 ('-', '.',
// and '\0') + number of decimal digits to represent maxfloat (which
// is maximum exponent + 1). (99+3+1 then rounded to 120 for "extra
// expenses", such as locale-dependent stuff)
//
#define MAX_ITEM        (120 + l_mathlim(MAX_10_EXP))


// valid flags in a format specification
#define FLAGS	"-+ #0"

//
// maximum size of each format specification (such as "%-099.99d")
//
#define MAX_FORMAT	32


static void addquoted(lhatL_Buffer *b, const char *s, size_t len)
{
    lhatL_addchar(b, '"');
    while(len--) {
        if(*s == '"' || *s == '\\' || *s == '\n') {
            lhatL_addchar(b, '\\');
            lhatL_addchar(b, *s);
        }
        else if(iscntrl(uchar(*s))) {
            char buff[10];
            if(!isdigit(uchar(*(s + 1))))
                l_sprintf(buff, sizeof(buff), "\\%d", (int)uchar(*s));
            else
                l_sprintf(buff, sizeof(buff), "\\%03d", (int)uchar(*s));
            lhatL_addstring(b, buff);
        }
        else
            lhatL_addchar(b, *s);
        s++;
    }
    lhatL_addchar(b, '"');
}


//
// Ensures the 'buff' string uses a dot as the radix character.
//
static void checkdp(char *buff, int nb)
{
    if(memchr(buff, '.', nb) == NULL) {  // no dot?
        char point = lhat_getlocaledecpoint();  // try locale point
        char *ppoint = (char *)memchr(buff, point, nb);
        if(ppoint) *ppoint = '.';  // change it to a dot
    }
}


static void addliteral(lhat_State *L, lhatL_Buffer *b, int arg)
{
    switch(lhat_type(L, arg)) {
    case LHAT_TSTRING: {
        size_t len;
        const char *s = lhat_tolstring(L, arg, &len);
        addquoted(b, s, len);
        break;
    }
    case LHAT_TNUMBER: {
        char *buff = lhatL_prepbuffsize(b, MAX_ITEM);
        int nb;
        if(!lhat_isinteger(L, arg)) {  // float?
            lhat_Number n = lhat_tonumber(L, arg);  // write as hexa ('%a')
            nb = lhat_number2strx(L, buff, MAX_ITEM, "%" LHAT_NUMBER_FRMLEN "a", n);
            checkdp(buff, nb);  // ensure it uses a dot
        }
        else {  // integers
            lhat_Integer n = lhat_tointeger(L, arg);
            const char *format = (n == LHAT_MININTEGER)  // corner case?
                ? "0x%" LHAT_INTEGER_FRMLEN "x"  // use hexa
                : LHAT_INTEGER_FMT;  // else use default format
            nb = l_sprintf(buff, MAX_ITEM, format, (LHATI_UACINT)n);
        }
        lhatL_addsize(b, nb);
        break;
    }
    case LHAT_TNIL: case LHAT_TBOOLEAN: {
        lhatL_tolstring(L, arg, NULL);
        lhatL_addvalue(b);
        break;
    }
    default:
        lhatL_argerror(L, arg, "value has no literal form");
    }
}


static const char *scanformat(lhat_State *L, const char *strfrmt, char *form)
{
    const char *p = strfrmt;
    while(*p != '\0' && strchr(FLAGS, *p) != NULL) p++;  // skip flags
    if((size_t)(p - strfrmt) >= sizeof(FLAGS) / sizeof(char))
        lhatL_error(L, "invalid format (repeated flags)");
    if(isdigit(uchar(*p))) p++;  // skip width
    if(isdigit(uchar(*p))) p++;  // (2 digits at most)
    if(*p == '.') {
        p++;
        if(isdigit(uchar(*p))) p++;  // skip precision
        if(isdigit(uchar(*p))) p++;  // (2 digits at most)
    }
    if(isdigit(uchar(*p)))
        lhatL_error(L, "invalid format (width or precision too long)");
    *(form++) = '%';
    memcpy(form, strfrmt, ((p - strfrmt) + 1) * sizeof(char));
    form += (p - strfrmt) + 1;
    *form = '\0';
    return p;
}


//
// add length modifier into formats
//
static void addlenmod(char *form, const char *lenmod)
{
    size_t l = strlen(form);
    size_t lm = strlen(lenmod);
    char spec = form[l - 1];
    strcpy(form + l - 1, lenmod);
    form[l + lm - 1] = spec;
    form[l + lm] = '\0';
}


static int str_format(lhat_State *L)
{
    int top = lhat_gettop(L);
    int arg = 1;
    size_t sfl;
    const char *strfrmt = lhatL_checklstring(L, arg, &sfl);
    const char *strfrmt_end = strfrmt + sfl;
    lhatL_Buffer b;
    lhatL_buffinit(&b, L);
    while(strfrmt < strfrmt_end) {
        if(*strfrmt != L_ESC)
            lhatL_addchar(&b, *strfrmt++);
        else if(*++strfrmt == L_ESC)
            lhatL_addchar(&b, *strfrmt++);  // %%
        else { // format item
            char form[MAX_FORMAT];  // to store the format ('%...')
            char *buff = lhatL_prepbuffsize(&b, MAX_ITEM);  // to put formatted item
            int nb = 0;  // number of bytes in added item
            if(++arg > top)
                lhatL_argerror(L, arg, "no value");
            strfrmt = scanformat(L, strfrmt, form);
            switch(*strfrmt++) {
            case 'c': {
                nb = l_sprintf(buff, MAX_ITEM, form, (int)lhatL_checkinteger(L, arg));
                break;
            }
            case 'd': case 'i':
            case 'o': case 'u': case 'x': case 'X': {
                lhat_Integer n = lhatL_checkinteger(L, arg);
                addlenmod(form, LHAT_INTEGER_FRMLEN);
                nb = l_sprintf(buff, MAX_ITEM, form, (LHATI_UACINT)n);
                break;
            }
            case 'a': case 'A':
                addlenmod(form, LHAT_NUMBER_FRMLEN);
                nb = lhat_number2strx(L, buff, MAX_ITEM, form,
                    lhatL_checknumber(L, arg));
                break;
            case 'e': case 'E': case 'f':
            case 'g': case 'G': {
                lhat_Number n = lhatL_checknumber(L, arg);
                addlenmod(form, LHAT_NUMBER_FRMLEN);
                nb = l_sprintf(buff, MAX_ITEM, form, (LHATI_UACNUMBER)n);
                break;
            }
            case 'q': {
                addliteral(L, &b, arg);
                break;
            }
            case 's': {
                size_t l;
                const char *s = lhatL_tolstring(L, arg, &l);
                if(form[2] == '\0')  // no modifiers?
                    lhatL_addvalue(&b);  // keep entire string
                else {
                    lhatL_argcheck(L, l == strlen(s), arg, "string contains zeros");
                    if(!strchr(form, '.') && l >= 100) {
                        // no precision and string is too long to be formatted
                        lhatL_addvalue(&b);  // keep entire string
                    }
                    else {  // format the string into 'buff'
                        nb = l_sprintf(buff, MAX_ITEM, form, s);
                        lhat_pop(L, 1);  // remove result from 'lhatL_tolstring'
                    }
                }
                break;
            }
            default: {  // also treat cases 'pnLlh'
                return lhatL_error(L, "invalid option '%%%c' to 'format'",
                    *(strfrmt - 1));
            }
            }
            lhat_assert(nb < MAX_ITEM);
            lhatL_addsize(&b, nb);
        }
    }
    lhatL_pushresult(&b);
    return 1;
}

// }======================================================


//
// {======================================================
// PACK/UNPACK
// =======================================================
//


// value used for padding
#if !defined(LHATL_PACKPADBYTE)
#define LHATL_PACKPADBYTE		0x00
#endif

// maximum size for the binary representation of an integer
#define MAXINTSIZE	16

// number of bits in a character
#define NB	CHAR_BIT

// mask for one character (NB 1's)
#define MC	((1 << NB) - 1)

// size of a lhat_Integer
#define SZINT	((int)sizeof(lhat_Integer))


// dummy union to get native endianness
static const union {
    int dummy;
    char little;  // true iff machine is little endian
} nativeendian = { 1 };


// dummy structure to get native alignment requirements
struct cD {
    char c;
    union { double d; void *p; lhat_Integer i; lhat_Number n; } u;
};

#define MAXALIGN	(offsetof(struct cD, u))


//
// Union for serializing floats
//
typedef union Ftypes {
    float f;
    double d;
    lhat_Number n;
    char buff[5 * sizeof(lhat_Number)];  // enough for any float type
} Ftypes;


//
// information to pack/unpack stuff
//
typedef struct Header {
    lhat_State *L;
    int islittle;
    int maxalign;
} Header;


//
// options for pack/unpack
//
typedef enum KOption {
    Kint,		// signed integers
    Kuint,	// unsigned integers
    Kfloat,	// floating-point numbers
    Kchar,	// fixed-length strings
    Kstring,	// strings with prefixed length
    Kzstr,	// zero-terminated strings
    Kpadding,	// padding
    Kpaddalign,	// padding for alignment
    Knop		// no-op (configuration or spaces)
} KOption;


//
// Read an integer numeral from string 'fmt' or return 'df' if
// there is no numeral
//
static int digit(int c) { return '0' <= c && c <= '9'; }

static int getnum(const char **fmt, int df)
{
    if(!digit(**fmt))  // no number?
        return df;  // return default value
    else {
        int a = 0;
        do {
            a = a * 10 + (*((*fmt)++) - '0');
        } while(digit(**fmt) && a <= ((int)MAXSIZE - 9) / 10);
        return a;
    }
}


//
// Read an integer numeral and raises an error if it is larger
// than the maximum size for integers.
//
static int getnumlimit(Header *h, const char **fmt, int df)
{
    int sz = getnum(fmt, df);
    if(sz > MAXINTSIZE || sz <= 0)
        lhatL_error(h->L, "integral size (%d) out of limits [1,%d]", sz, MAXINTSIZE);
    return sz;
}


//
// Initialize Header
//
static void initheader(lhat_State *L, Header *h)
{
    h->L = L;
    h->islittle = nativeendian.little;
    h->maxalign = 1;
}


//
// Read and classify next option. 'size' is filled with option's size.
//
static KOption getoption(Header *h, const char **fmt, int *size)
{
    int opt = *((*fmt)++);
    *size = 0;  // default
    switch(opt) {
    case 'b': *size = sizeof(char); return Kint;
    case 'B': *size = sizeof(char); return Kuint;
    case 'h': *size = sizeof(short); return Kint;
    case 'H': *size = sizeof(short); return Kuint;
    case 'l': *size = sizeof(long); return Kint;
    case 'L': *size = sizeof(long); return Kuint;
    case 'j': *size = sizeof(lhat_Integer); return Kint;
    case 'J': *size = sizeof(lhat_Integer); return Kuint;
    case 'T': *size = sizeof(size_t); return Kuint;
    case 'f': *size = sizeof(float); return Kfloat;
    case 'd': *size = sizeof(double); return Kfloat;
    case 'n': *size = sizeof(lhat_Number); return Kfloat;
    case 'i': *size = getnumlimit(h, fmt, sizeof(int)); return Kint;
    case 'I': *size = getnumlimit(h, fmt, sizeof(int)); return Kuint;
    case 's': *size = getnumlimit(h, fmt, sizeof(size_t)); return Kstring;
    case 'c':
        *size = getnum(fmt, -1);
        if(*size == -1)
            lhatL_error(h->L, "missing size for format option 'c'");
        return Kchar;
    case 'z': return Kzstr;
    case 'x': *size = 1; return Kpadding;
    case 'X': return Kpaddalign;
    case ' ': break;
    case '<': h->islittle = 1; break;
    case '>': h->islittle = 0; break;
    case '=': h->islittle = nativeendian.little; break;
    case '!': h->maxalign = getnumlimit(h, fmt, MAXALIGN); break;
    default: lhatL_error(h->L, "invalid format option '%c'", opt);
    }
    return Knop;
}


//
// Read, classify, and fill other details about the next option.
// 'psize' is filled with option's size, 'notoalign' with its
// alignment requirements.
// Local variable 'size' gets the size to be aligned. (Kpadal option
// always gets its full alignment, other options are limited by
// the maximum alignment ('maxalign'). Kchar option needs no alignment
// despite its size.
//
static KOption getdetails(Header *h, size_t totalsize, const char **fmt, int *psize, int *ntoalign)
{
    KOption opt = getoption(h, fmt, psize);
    int align = *psize;  // usually, alignment follows size
    if(opt == Kpaddalign) {  // 'X' gets alignment from following option
        if(**fmt == '\0' || getoption(h, fmt, &align) == Kchar || align == 0)
            lhatL_argerror(h->L, 1, "invalid next option for option 'X'");
    }
    if(align <= 1 || opt == Kchar)  // need no alignment?
        *ntoalign = 0;
    else {
        if(align > h->maxalign)  // enforce maximum alignment
            align = h->maxalign;
        if((align & (align - 1)) != 0)  // is 'align' not a power of 2?
            lhatL_argerror(h->L, 1, "format asks for alignment not power of 2");
        *ntoalign = (align - (int)(totalsize & (align - 1))) & (align - 1);
    }
    return opt;
}


//
// Pack integer 'n' with 'size' bytes and 'islittle' endianness.
// The final 'if' handles the case when 'size' is larger than
// the size of a L^ integer, correcting the extra sign-extension
// bytes if necessary (by default they would be zeros).
//
static void packint(lhatL_Buffer *b, lhat_Unsigned n, int islittle, int size, int neg)
{
    char *buff = lhatL_prepbuffsize(b, size);
    buff[islittle ? 0 : size - 1] = (char)(n & MC);  // first byte
    for(int i = 1; i < size; i++) {
        n >>= NB;
        buff[islittle ? i : size - 1 - i] = (char)(n & MC);
    }
    if(neg && size > SZINT) {  // negative number need sign extension?
        for(int i = SZINT; i < size; i++)  // correct extra bytes
            buff[islittle ? i : size - 1 - i] = (char)MC;
    }
    lhatL_addsize(b, size);  // add result to buffer
}


//
// Copy 'size' bytes from 'src' to 'dest', correcting endianness if
// given 'islittle' is different from native endianness.
//
static void copywithendian(volatile char *dest, volatile const char *src,
    int size, int islittle)
{
    if(islittle == nativeendian.little) {
        while(size-- != 0)
            *(dest++) = *(src++);
    }
    else {
        dest += size - 1;
        while(size-- != 0)
            *(dest--) = *(src++);
    }
}


static int str_pack(lhat_State *L)
{
    const char *fmt = lhatL_checkstring(L, 1);  // format string
    int arg = 1;  // current argument to pack
    size_t totalsize = 0;  // accumulate total size of result
    Header h;
    initheader(L, &h);
    lhat_pushnil(L);  // mark to separate arguments from string buffer
    lhatL_Buffer b;
    lhatL_buffinit(&b, L);
    while(*fmt != '\0') {
        int size, ntoalign;
        KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
        totalsize += ntoalign + size;
        while(ntoalign-- > 0)
            lhatL_addchar(&b, LHATL_PACKPADBYTE);  // fill alignment
        arg++;
        switch(opt) {
        case Kint: {  // signed integers
            lhat_Integer n = lhatL_checkinteger(L, arg);
            if(size < SZINT) {  // need overflow check?
                lhat_Integer lim = (lhat_Integer)1 << ((size * NB) - 1);
                lhatL_argcheck(L, -lim <= n && n < lim, arg, "integer overflow");
            }
            packint(&b, (lhat_Unsigned)n, h.islittle, size, (n < 0));
            break;
        }
        case Kuint: {  // unsigned integers
            lhat_Integer n = lhatL_checkinteger(L, arg);
            if(size < SZINT)  // need overflow check?
                lhatL_argcheck(L, (lhat_Unsigned)n < ((lhat_Unsigned)1 << (size * NB)),
                    arg, "unsigned overflow");
            packint(&b, (lhat_Unsigned)n, h.islittle, size, 0);
            break;
        }
        case Kfloat: {  // floating-point options
            volatile Ftypes u;
            char *buff = lhatL_prepbuffsize(&b, size);
            lhat_Number n = lhatL_checknumber(L, arg);  // get argument
            if(size == sizeof(u.f)) u.f = (float)n;  // copy it into 'u'
            else if(size == sizeof(u.d)) u.d = (double)n;
            else u.n = n;
            // move 'u' to final result, correcting endianness if needed
            copywithendian(buff, u.buff, size, h.islittle);
            lhatL_addsize(&b, size);
            break;
        }
        case Kchar: {  // fixed-size string
            size_t len;
            const char *s = lhatL_checklstring(L, arg, &len);
            lhatL_argcheck(L, len <= (size_t)size, arg,
                "string longer than given size");
            lhatL_addlstring(&b, s, len);  // add string
            while(len++ < (size_t)size)  // pad extra space
                lhatL_addchar(&b, LHATL_PACKPADBYTE);
            break;
        }
        case Kstring: {  // strings with length count
            size_t len;
            const char *s = lhatL_checklstring(L, arg, &len);
            lhatL_argcheck(L, size >= (int)sizeof(size_t) ||
                len < ((size_t)1 << (size * NB)),
                arg, "string length does not fit in given size");
            packint(&b, (lhat_Unsigned)len, h.islittle, size, 0);  // pack length
            lhatL_addlstring(&b, s, len);
            totalsize += len;
            break;
        }
        case Kzstr: {  // zero-terminated string
            size_t len;
            const char *s = lhatL_checklstring(L, arg, &len);
            lhatL_argcheck(L, strlen(s) == len, arg, "string contains zeros");
            lhatL_addlstring(&b, s, len);
            lhatL_addchar(&b, '\0');  // add zero at the end
            totalsize += len + 1;
            break;
        }
        case Kpadding: lhatL_addchar(&b, LHATL_PACKPADBYTE);  // FALLTHROUGH
        case Kpaddalign: case Knop:
            arg--;  // undo increment
            break;
        }
    }
    lhatL_pushresult(&b);
    return 1;
}


static int str_packsize(lhat_State *L)
{
    Header h;
    const char *fmt = lhatL_checkstring(L, 1);  // format string
    size_t totalsize = 0;  // accumulate total size of result
    initheader(L, &h);
    while(*fmt != '\0') {
        int size, ntoalign;
        KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
        size += ntoalign;  // total space used by option
        lhatL_argcheck(L, totalsize <= MAXSIZE - size, 1,
            "format result too large");
        totalsize += size;
        switch(opt) {
        case Kstring:  // strings with length count
        case Kzstr:    // zero-terminated string
            lhatL_argerror(L, 1, "variable-length format");
            // call never return, but to avoid warnings:// FALLTHROUGH
        default:  break;
        }
    }
    lhat_pushinteger(L, (lhat_Integer)totalsize);
    return 1;
}


//
// Unpack an integer with 'size' bytes and 'islittle' endianness.
// If size is smaller than the size of a L^ integer and integer
// is signed, must do sign extension (propagating the sign to the
// higher bits); if size is larger than the size of a L^ integer,
// it must check the unread bytes to see whether they do not cause an
// overflow.
//
static lhat_Integer unpackint(lhat_State *L, const char *str,
    int islittle, int size, int issigned)
{
    lhat_Unsigned res = 0;
    int i;
    int limit = (size <= SZINT) ? size : SZINT;
    for(i = limit - 1; i >= 0; i--) {
        res <<= NB;
        res |= (lhat_Unsigned)(unsigned char)str[islittle ? i : size - 1 - i];
    }
    if(size < SZINT) {  // real size smaller than lhat_Integer?
        if(issigned) {  // needs sign extension?
            lhat_Unsigned mask = (lhat_Unsigned)1 << (size*NB - 1);
            res = ((res ^ mask) - mask);  // do sign extension
        }
    }
    else if(size > SZINT) {  // must check unread bytes
        int mask = (!issigned || (lhat_Integer)res >= 0) ? 0 : MC;
        for(i = limit; i < size; i++) {
            if((unsigned char)str[islittle ? i : size - 1 - i] != mask)
                lhatL_error(L, "%d-byte integer does not fit into L^ Integer", size);
        }
    }
    return (lhat_Integer)res;
}


static int str_unpack(lhat_State *L)
{
    const char *fmt = lhatL_checkstring(L, 1);
    size_t ld;
    const char *data = lhatL_checklstring(L, 2, &ld);
    size_t pos = (size_t)posrelat(lhatL_optinteger(L, 3, 1), ld) - 1;
    int n = 0;  // number of results
    lhatL_argcheck(L, pos <= ld, 3, "initial position out of string");
    Header h;
    initheader(L, &h);
    while(*fmt != '\0') {
        int size, ntoalign;
        KOption opt = getdetails(&h, pos, &fmt, &size, &ntoalign);
        if((size_t)ntoalign + size > ~pos || pos + ntoalign + size > ld)
            lhatL_argerror(L, 2, "data string too short");
        pos += ntoalign;  // skip alignment
        // stack space for item + next position
        lhatL_checkstack(L, 2, "too many results");
        n++;
        switch(opt) {
        case Kint:
        case Kuint: {
            lhat_Integer res = unpackint(L, data + pos, h.islittle, size,
                (opt == Kint));
            lhat_pushinteger(L, res);
            break;
        }
        case Kfloat: {
            volatile Ftypes u;
            lhat_Number num;
            copywithendian(u.buff, data + pos, size, h.islittle);
            if(size == sizeof(u.f)) num = (lhat_Number)u.f;
            else if(size == sizeof(u.d)) num = (lhat_Number)u.d;
            else num = u.n;
            lhat_pushnumber(L, num);
            break;
        }
        case Kchar: {
            lhat_pushlstring(L, data + pos, size);
            break;
        }
        case Kstring: {
            size_t len = (size_t)unpackint(L, data + pos, h.islittle, size, 0);
            lhatL_argcheck(L, pos + len + size <= ld, 2, "data string too short");
            lhat_pushlstring(L, data + pos + size, len);
            pos += len;  // skip string
            break;
        }
        case Kzstr: {
            size_t len = (int)strlen(data + pos);
            lhat_pushlstring(L, data + pos, len);
            pos += len + 1;  // skip string plus final '\0'
            break;
        }
        case Kpaddalign: case Kpadding: case Knop:
            n--;  // undo increment
            break;
        }
        pos += size;
    }
    lhat_pushinteger(L, pos + 1);  // next position
    return n + 1;
}

// }======================================================


static const lhatL_Reg strlib[] = {
    { "byte", str_byte },
    { "char", str_char },
    { "dump", str_dump },
    { "find", str_find },
    { "format", str_format },
    { "gmatch", gmatch },
    { "gsub", str_gsub },
    { "len", str_len },
    { "lower", str_lower },
    { "match", str_match },
    { "rep", str_rep },
    { "reverse", str_reverse },
    { "sub", str_sub },
    { "upper", str_upper },
    { "pack", str_pack },
    { "packsize", str_packsize },
    { "unpack", str_unpack },
    { NULL, NULL }
};


static void createmetatable(lhat_State *L)
{
    lhat_createtable(L, 0, 1);  // table to be metatable for strings
    lhat_pushliteral(L, "");  // dummy string
    lhat_pushvalue(L, -2);  // copy table
    lhat_setmetatable(L, -2);  // set table as metatable for strings
    lhat_pop(L, 1);  // pop dummy string
    lhat_pushvalue(L, -2);  // get string library
    lhat_setfield(L, -2, "__index");  // metatable.__index = string
    lhat_pop(L, 1);  // pop metatable
}


//
// Open string library
//
LHATMOD_API int lhatopen_string(lhat_State *L)
{
    lhatL_newlib(L, strlib);
    createmetatable(L);
    return 1;
}
