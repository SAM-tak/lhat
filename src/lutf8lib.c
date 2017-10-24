//
// Standard library for UTF-8 manipulation
// See Copyright Notice in lhat.h
//

#define LHAT_LIB

#include "lprefix.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"

#define MAXUNICODE	0x10FFFF

#define iscont(p)	((*(p) & 0xC0) == 0x80)


// from strlib
// translate a relative string position: negative means back from end
static lhat_Integer u_posrelat(lhat_Integer pos, size_t len)
{
    if(pos >= 0) return pos;
    else if(0u - (size_t)pos > len) return 0;
    else return (lhat_Integer)len + pos + 1;
}


//
// Decode one UTF-8 sequence, returning NULL if byte sequence is invalid.
//
static const char *utf8_decode(const char *o, int *val)
{
    static const unsigned int limits[] = { 0xFF, 0x7F, 0x7FF, 0xFFFF };
    const unsigned char *s = (const unsigned char *)o;
    unsigned int c = s[0];
    unsigned int res = 0;  // final result
    if(c < 0x80)  // ascii?
        res = c;
    else {
        int count = 0;  // to count number of continuation bytes
        while(c & 0x40) {  // still have continuation bytes?
            int cc = s[++count];  // read next byte
            if((cc & 0xC0) != 0x80)  // not a continuation byte?
                return NULL;  // invalid byte sequence
            res = (res << 6) | (cc & 0x3F);  // add lower 6 bits from cont. byte
            c <<= 1;  // to test next bit
        }
        res |= ((c & 0x7F) << (count * 5));  // add first byte
        if(count > 3 || res > MAXUNICODE || res <= limits[count])
            return NULL;  // invalid byte sequence
        s += count;  // skip continuation bytes read
    }
    if(val) *val = res;
    return (const char *)s + 1;  // +1 to include first byte
}


//
// utf8len(s [, i [, j]]) --> number of characters that start in the
// range [i,j], or nil + current position if 's' is not well formed in
// that interval
//
static int utflen(lhat_State *L)
{
    int n = 0;
    size_t len;
    const char *s = lhatL_checklstring(L, 1, &len);
    lhat_Integer posi = u_posrelat(lhatL_optinteger(L, 2, 1), len);
    lhat_Integer posj = u_posrelat(lhatL_optinteger(L, 3, -1), len);
    lhatL_argcheck(L, 1 <= posi && --posi <= (lhat_Integer)len, 2,
        "initial position out of string");
    lhatL_argcheck(L, --posj < (lhat_Integer)len, 3,
        "final position out of string");
    while(posi <= posj) {
        const char *s1 = utf8_decode(s + posi, NULL);
        if(s1 == NULL) {  // conversion error?
            lhat_pushnil(L);  // return nil ...
            lhat_pushinteger(L, posi + 1);  // ... and current position
            return 2;
        }
        posi = s1 - s;
        n++;
    }
    lhat_pushinteger(L, n);
    return 1;
}


//
// codepoint(s, [i, [j]])  -> returns codepoints for all characters
// that start in the range [i,j]
//
static int codepoint(lhat_State *L)
{
    size_t len;
    const char *s = lhatL_checklstring(L, 1, &len);
    lhat_Integer posi = u_posrelat(lhatL_optinteger(L, 2, 1), len);
    lhat_Integer pose = u_posrelat(lhatL_optinteger(L, 3, posi), len);
    int n;
    const char *se;
    lhatL_argcheck(L, posi >= 1, 2, "out of range");
    lhatL_argcheck(L, pose <= (lhat_Integer)len, 3, "out of range");
    if(posi > pose) return 0;  // empty interval; return no values
    if(pose - posi >= INT_MAX)  // (lhat_Integer -> int) overflow?
        return lhatL_error(L, "string slice too long");
    n = (int)(pose - posi) + 1;
    lhatL_checkstack(L, n, "string slice too long");
    n = 0;
    se = s + pose;
    for(s += posi - 1; s < se;) {
        int code;
        s = utf8_decode(s, &code);
        if(s == NULL)
            return lhatL_error(L, "invalid UTF-8 code");
        lhat_pushinteger(L, code);
        n++;
    }
    return n;
}


static void pushutfchar(lhat_State *L, int arg)
{
    lhat_Integer code = lhatL_checkinteger(L, arg);
    lhatL_argcheck(L, 0 <= code && code <= MAXUNICODE, arg, "value out of range");
    lhat_pushfstring(L, "%U", (long)code);
}


//
// utfchar(n1, n2, ...)  -> char(n1)..char(n2)...
//
static int utfchar(lhat_State *L)
{
    int n = lhat_gettop(L);  // number of arguments
    if(n == 1)  // optimize common case of single char
        pushutfchar(L, 1);
    else {
        int i;
        lhatL_Buffer b;
        lhatL_buffinit(&b, L);
        for(i = 1; i <= n; i++) {
            pushutfchar(L, i);
            lhatL_addvalue(&b);
        }
        lhatL_pushresult(&b);
    }
    return 1;
}


//
// offset(s, n, [i])  -> index where n-th character counting from
//   position 'i' starts; 0 means character at 'i'.
//
static int byteoffset(lhat_State *L)
{
    size_t len;
    const char *s = lhatL_checklstring(L, 1, &len);
    lhat_Integer n = lhatL_checkinteger(L, 2);
    lhat_Integer posi = (n >= 0) ? 1 : len + 1;
    posi = u_posrelat(lhatL_optinteger(L, 3, posi), len);
    lhatL_argcheck(L, 1 <= posi && --posi <= (lhat_Integer)len, 3,
        "position out of range");
    if(n == 0) {
        // find beginning of current byte sequence
        while(posi > 0 && iscont(s + posi)) posi--;
    }
    else {
        if(iscont(s + posi))
            lhatL_error(L, "initial position is a continuation byte");
        if(n < 0) {
            while(n < 0 && posi > 0) {  // move back
                do {  // find beginning of previous character
                    posi--;
                } while(posi > 0 && iscont(s + posi));
                n++;
            }
        }
        else {
            n--;  // do not move for 1st character
            while(n > 0 && posi < (lhat_Integer)len) {
                do {  // find beginning of next character
                    posi++;
                } while(iscont(s + posi));  // (cannot pass final '\0')
                n--;
            }
        }
    }
    if(n == 0)  // did it find given character?
        lhat_pushinteger(L, posi + 1);
    else  // no such character
        lhat_pushnil(L);
    return 1;
}


static int iter_aux(lhat_State *L)
{
    size_t len;
    const char *s = lhatL_checklstring(L, 1, &len);
    lhat_Integer n = lhat_tointeger(L, 2) - 1;
    if(n < 0)  // first iteration?
        n = 0;  // start from here
    else if(n < (lhat_Integer)len) {
        n++;  // skip current byte
        while(iscont(s + n)) n++;  // and its continuations
    }
    if(n >= (lhat_Integer)len)
        return 0;  // no more codepoints
    else {
        int code;
        const char *next = utf8_decode(s + n, &code);
        if(next == NULL || iscont(next))
            return lhatL_error(L, "invalid UTF-8 code");
        lhat_pushinteger(L, n + 1);
        lhat_pushinteger(L, code);
        return 2;
    }
}


static int iter_codes(lhat_State *L)
{
    lhatL_checkstring(L, 1);
    lhat_pushcfunction(L, iter_aux);
    lhat_pushvalue(L, 1);
    lhat_pushinteger(L, 0);
    return 3;
}


// pattern to match a single UTF-8 character
#define UTF8PATT	"[\0-\x7F\xC2-\xF4][\x80-\xBF]*"


static const lhatL_Reg funcs[] = {
    { "offset", byteoffset },
    { "codepoint", codepoint },
    { "char", utfchar },
    { "len", utflen },
    { "codes", iter_codes },
    // placeholders
    { "charpattern", NULL },
    { NULL, NULL }
};


LHATMOD_API int lhatopen_utf8(lhat_State *L)
{
    lhatL_newlib(L, funcs);
    lhat_pushlstring(L, UTF8PATT, sizeof(UTF8PATT) / sizeof(char) - 1);
    lhat_setfield(L, -2, "charpattern");
    return 1;
}
