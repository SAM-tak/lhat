//
// Some generic functions over L^ objects
// See Copyright Notice in lhat.h
//

#define LHAT_CORE

#include "lprefix.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lhat.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "lvm.h"


LHATI_DDEF const TValue lhatO_nilobject_ = { NILCONSTANT };


//
// converts an integer to a "floating point byte", represented as
// (eeeeexxx), where the real value is (1xxx) * 2^(eeeee - 1) if
// eeeee != 0 and (xxx) otherwise.
//
int lhatO_int2fb(unsigned int x)
{
    int e = 0;  // exponent
    if(x < 8) return x;
    while(x >= (8 << 4)) {  // coarse steps
        x = (x + 0xf) >> 4;  // x = ceil(x / 16)
        e += 4;
    }
    while(x >= (8 << 1)) {  // fine steps
        x = (x + 1) >> 1;  // x = ceil(x / 2)
        e++;
    }
    return ((e + 1) << 3) | (cast_int(x) - 8);
}


// converts back
int lhatO_fb2int(int x)
{
    return (x < 8) ? x : ((x & 7) + 8) << ((x >> 3) - 1);
}


//
// Computes ceil(log2(x))
//
int lhatO_ceillog2(unsigned int x)
{
    static const lu_byte log_2[256] = {  // log_2[i] = ceil(log2(i - 1))
        0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8
    };
    int l = 0;
    x--;
    while(x >= 256) { l += 8; x >>= 8; }
    return l + log_2[x];
}


static lhat_Integer intarith(lhat_State *L, int op, lhat_Integer v1, lhat_Integer v2)
{
    switch(op) {
    case LHAT_OPADD: return intop(+, v1, v2);
    case LHAT_OPSUB:return intop(-, v1, v2);
    case LHAT_OPMUL:return intop(*, v1, v2);
    case LHAT_OPMOD: return lhatV_mod(L, v1, v2);
    case LHAT_OPIDIV: return lhatV_div(L, v1, v2);
    case LHAT_OPBAND: return intop(&, v1, v2);
    case LHAT_OPBOR: return intop(| , v1, v2);
    case LHAT_OPBXOR: return intop(^, v1, v2);
    case LHAT_OPSHL: return lhatV_shiftl(v1, v2);
    case LHAT_OPSHR: return lhatV_shiftl(v1, -v2);
    case LHAT_OPUNM: return intop(-, 0, v1);
    case LHAT_OPBNOT: return intop(^, ~l_castS2U(0), v1);
    default: lhat_assert(0); return 0;
    }
}


static lhat_Number numarith(lhat_State *L, int op, lhat_Number v1, lhat_Number v2)
{
    switch(op) {
    case LHAT_OPADD: return lhati_numadd(L, v1, v2);
    case LHAT_OPSUB: return lhati_numsub(L, v1, v2);
    case LHAT_OPMUL: return lhati_nummul(L, v1, v2);
    case LHAT_OPDIV: return lhati_numdiv(L, v1, v2);
    case LHAT_OPPOW: return lhati_numpow(L, v1, v2);
    case LHAT_OPIDIV: return lhati_numidiv(L, v1, v2);
    case LHAT_OPUNM: return lhati_numunm(L, v1);
    case LHAT_OPMOD: {
        lhat_Number m;
        lhati_nummod(L, v1, v2, m);
        return m;
    }
    default: lhat_assert(0); return 0;
    }
}


void lhatO_arith(lhat_State *L, int op, const TValue *p1, const TValue *p2, TValue *res)
{
    switch(op) {
    case LHAT_OPBAND: case LHAT_OPBOR: case LHAT_OPBXOR:
    case LHAT_OPSHL: case LHAT_OPSHR:
    case LHAT_OPBNOT: {  // operate only on integers
        lhat_Integer i1; lhat_Integer i2;
        if(tointeger(p1, &i1) && tointeger(p2, &i2)) {
            setivalue(res, intarith(L, op, i1, i2));
            return;
        }
        else break;  // go to the end
    }
    case LHAT_OPDIV: case LHAT_OPPOW: {  // operate only on floats
        lhat_Number n1; lhat_Number n2;
        if(tonumber(p1, &n1) && tonumber(p2, &n2)) {
            setfltvalue(res, numarith(L, op, n1, n2));
            return;
        }
        else break;  // go to the end
    }
    default: {  // other operations
        lhat_Number n1; lhat_Number n2;
        if(ttisinteger(p1) && ttisinteger(p2)) {
            setivalue(res, intarith(L, op, ivalue(p1), ivalue(p2)));
            return;
        }
        else if(tonumber(p1, &n1) && tonumber(p2, &n2)) {
            setfltvalue(res, numarith(L, op, n1, n2));
            return;
        }
        else break;  // go to the end
    }
    }
    // could not perform raw operation; try metamethod
    lhat_assert(L != NULL);  // should not fail when folding (compile time)
    lhatT_tryBinMM(L, p1, p2, res, cast(MetaMethod, (op - LHAT_OPADD) + MM_ADD));
}


int lhatO_hexavalue(int c)
{
    if(lisdigit(c)) return c - '0';
    else return (ltolower(c) - 'a') + 10;
}


static int isneg(const char **s)
{
    if(**s == '-') { (*s)++; return 1; }
    else if(**s == '+') (*s)++;
    return 0;
}



//
// {==================================================================
// L^'s implementation for 'lhat_strx2number'
// ===================================================================
//

#if !defined(lhat_strx2number)

// maximum number of significant digits to read (to avoid overflows
even with single floats)
#define MAXSIGDIG	30

//
// convert an hexadecimal numeric string to a number, following
// C99 specification for 'strtod'
//
static lhat_Number lhat_strx2number(const char *s, char **endptr)
{
    int dot = lhat_getlocaledecpoint();
    lhat_Number r = 0.0;  // result (accumulator)
    int sigdig = 0;  // number of significant digits
    int nosigdig = 0;  // number of non-significant digits
    int e = 0;  // exponent correction
    int neg;  // 1 if number is negative
    int hasdot = 0;  // true after seen a dot
    *endptr = cast(char *, s);  // nothing is valid yet
    while(lisspace(cast_uchar(*s))) s++;  // skip initial spaces
    neg = isneg(&s);  // check signal
    if(!(*s == '0' && (*(s + 1) == 'x' || *(s + 1) == 'X')))  // check '0x'
        return 0.0;  // invalid format (no '0x')
    for(s += 2; ; s++) {  // skip '0x' and read numeral
        if(*s == dot) {
            if(hasdot) break;  // second dot? stop loop
            else hasdot = 1;
        }
        else if(lisxdigit(cast_uchar(*s))) {
            if(sigdig == 0 && *s == '0')  // non-significant digit (zero)?
                nosigdig++;
            else if(++sigdig <= MAXSIGDIG)  // can read it without overflow?
                r = (r * cast_num(16.0)) + lhatO_hexavalue(*s);
            else e++; // too many digits; ignore, but still count for exponent
            if(hasdot) e--;  // decimal digit? correct exponent
        }
        else break;  // neither a dot nor a digit
    }
    if(nosigdig + sigdig == 0)  // no digits?
        return 0.0;  // invalid format
    *endptr = cast(char *, s);  // valid up to here
    e *= 4;  // each digit multiplies/divides value by 2^4
    if(*s == 'p' || *s == 'P') {  // exponent part?
        int exp1 = 0;  // exponent value
        int neg1;  // exponent signal
        s++;  // skip 'p'
        neg1 = isneg(&s);  // signal
        if(!lisdigit(cast_uchar(*s)))
            return 0.0;  // invalid; must have at least one digit
        while(lisdigit(cast_uchar(*s)))  // read exponent
            exp1 = exp1 * 10 + *(s++) - '0';
        if(neg1) exp1 = -exp1;
        e += exp1;
        *endptr = cast(char *, s);  // valid up to here
    }
    if(neg) r = -r;
    return l_mathop(ldexp)(r, e);
}

#endif
// }======================================================


// maximum length of a numeral
#if !defined (L_MAXLENNUM)
#define L_MAXLENNUM	200
#endif

static const char *l_str2dloc(const char *s, lhat_Number *result, int mode)
{
    char *endptr;
    *result = (mode == 'x') ? lhat_strx2number(s, &endptr)  // try to convert
        : lhat_str2number(s, &endptr);
    if(endptr == s) return NULL;  // nothing recognized?
    while(lisspace(cast_uchar(*endptr))) endptr++;  // skip trailing spaces
    return (*endptr == '\0') ? endptr : NULL;  // OK if no trailing characters
}


//
// Convert string 's' to a L^ number (put in 'result'). Return NULL
// on fail or the address of the ending '\0' on success.
// 'pmode' points to (and 'mode' contains) special things in the string:
// - 'x'/'X' means an hexadecimal numeral
// - 'n'/'N' means 'inf' or 'nan' (which should be rejected)
// - '.' just optimizes the search for the common case (nothing special)
// This function accepts both the current locale or a dot as the radix
// mark. If the convertion fails, it may mean number has a dot but
// locale accepts something else. In that case, the code copies 's'
// to a buffer (because 's' is read-only), changes the dot to the
// current locale radix mark, and tries to convert again.
//
static const char *l_str2d(const char *s, lhat_Number *result)
{
    const char *endptr;
    const char *pmode = strpbrk(s, ".xXnN");
    int mode = pmode ? ltolower(cast_uchar(*pmode)) : 0;
    if(mode == 'n')  // reject 'inf' and 'nan'
        return NULL;
    endptr = l_str2dloc(s, result, mode);  // try to convert
    if(endptr == NULL) {  // failed? may be a different locale
        char buff[L_MAXLENNUM + 1];
        const char *pdot = strchr(s, '.');
        if(strlen(s) > L_MAXLENNUM || pdot == NULL)
            return NULL;  // string too long or no dot; fail
        strcpy(buff, s);  // copy string to buffer
        buff[pdot - s] = lhat_getlocaledecpoint();  // correct decimal point
        endptr = l_str2dloc(buff, result, mode);  // try again
        if(endptr != NULL)
            endptr = s + (endptr - buff);  // make relative to 's'
    }
    return endptr;
}


#define MAXBY10		cast(lhat_Unsigned, LHAT_MAXINTEGER / 10)
#define MAXLASTD	cast_int(LHAT_MAXINTEGER % 10)

static const char *l_str2int(const char *s, lhat_Integer *result)
{
    lhat_Unsigned a = 0;
    int empty = 1;
    int neg;
    while(lisspace(cast_uchar(*s))) s++;  // skip initial spaces
    neg = isneg(&s);
    if(s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {  // hex?
        s += 2;  // skip '0x'
        for(; lisxdigit(cast_uchar(*s)); s++) {
            a = a * 16 + lhatO_hexavalue(*s);
            empty = 0;
        }
    }
    else {  // decimal
        for(; lisdigit(cast_uchar(*s)); s++) {
            int d = *s - '0';
            if(a >= MAXBY10 && (a > MAXBY10 || d > MAXLASTD + neg))  // overflow?
                return NULL;  // do not accept it (as integer)
            a = a * 10 + d;
            empty = 0;
        }
    }
    while(lisspace(cast_uchar(*s))) s++;  // skip trailing spaces
    if(empty || *s != '\0') return NULL;  // something wrong in the numeral
    else {
        *result = l_castU2S((neg) ? 0u - a : a);
        return s;
    }
}


size_t lhatO_str2num(const char *s, TValue *o)
{
    lhat_Integer i; lhat_Number n;
    const char *e;
    if((e = l_str2int(s, &i)) != NULL) {  // try as an integer
        setivalue(o, i);
    }
    else if((e = l_str2d(s, &n)) != NULL) {  // else try as a float
        setfltvalue(o, n);
    }
    else
        return 0;  // conversion failed
    return (e - s) + 1;  // success; return string size
}


int lhatO_utf8esc(char *buff, unsigned long x)
{
    int n = 1;  // number of bytes put in buffer (backwards)
    lhat_assert(x <= 0x10FFFF);
    if(x < 0x80)  // ascii?
        buff[UTF8BUFFSZ - 1] = cast(char, x);
    else {  // need continuation bytes
        unsigned int mfb = 0x3f;  // maximum that fits in first byte
        do {  // add continuation bytes
            buff[UTF8BUFFSZ - (n++)] = cast(char, 0x80 | (x & 0x3f));
            x >>= 6;  // remove added bits
            mfb >>= 1;  // now there is one less bit available in first byte
        } while(x > mfb);  // still needs continuation byte?
        buff[UTF8BUFFSZ - n] = cast(char, (~mfb << 1) | x);  // add first byte
    }
    return n;
}


// maximum length of the conversion of a number to a string
#define MAXNUMBER2STR	50


//
// Convert a number object to a string
//
void lhatO_tostring(lhat_State *L, StkId obj)
{
    char buff[MAXNUMBER2STR];
    size_t len;
    lhat_assert(ttisnumber(obj));
    if(ttisinteger(obj))
        len = lhat_integer2str(buff, sizeof(buff), ivalue(obj));
    else {
        len = lhat_number2str(buff, sizeof(buff), fltvalue(obj));
#if !defined(LHAT_COMPAT_FLOATSTRING)
        if(buff[strspn(buff, "-0123456789")] == '\0') {  // looks like an int?
            buff[len++] = lhat_getlocaledecpoint();
            buff[len++] = '0';  // adds '.0' to result
        }
#endif
    }
    setsvalue2s(L, obj, lhatS_newlstr(L, buff, len));
}


static void pushstr(lhat_State *L, const char *str, size_t l)
{
    setsvalue2s(L, L->top, lhatS_newlstr(L, str, l));
    lhatD_inctop(L);
}


//
// this function handles only '%d', '%c', '%f', '%p', and '%s'
// conventional formats, plus L^-specific '%I' and '%U'
//
const char *lhatO_pushvfstring(lhat_State *L, const char *fmt, va_list argp)
{
    int n = 0;
    for(;;) {
        const char *e = strchr(fmt, '%');
        if(e == NULL) break;
        pushstr(L, fmt, e - fmt);
        switch(*(e + 1)) {
        case 's': {  // zero-terminated string
            const char *s = va_arg(argp, char *);
            if(s == NULL) s = "(null)";
            pushstr(L, s, strlen(s));
            break;
        }
        case 'c': {  // an 'int' as a character
            char buff = cast(char, va_arg(argp, int));
            if(lisprint(cast_uchar(buff)))
                pushstr(L, &buff, 1);
            else  // non-printable character; print its code
                lhatO_pushfstring(L, "<\\%d>", cast_uchar(buff));
            break;
        }
        case 'd': {  // an 'int'
            setivalue(L->top, va_arg(argp, int));
            goto top2str;
        }
        case 'I': {  // a 'lhat_Integer'
            setivalue(L->top, cast(lhat_Integer, va_arg(argp, l_uacInt)));
            goto top2str;
        }
        case 'f': {  // a 'lhat_Number'
            setfltvalue(L->top, cast_num(va_arg(argp, l_uacNumber)));
        top2str:  // convert the top element to a string
            lhatD_inctop(L);
            lhatO_tostring(L, L->top - 1);
            break;
        }
        case 'p': {  // a pointer
            char buff[4 * sizeof(void *) + 8]; // should be enough space for a '%p'
            int l = l_sprintf(buff, sizeof(buff), "%p", va_arg(argp, void *));
            pushstr(L, buff, l);
            break;
        }
        case 'U': {  // an 'int' as a UTF-8 sequence
            char buff[UTF8BUFFSZ];
            int l = lhatO_utf8esc(buff, cast(long, va_arg(argp, long)));
            pushstr(L, buff + UTF8BUFFSZ - l, l);
            break;
        }
        case '%': {
            pushstr(L, "%", 1);
            break;
        }
        default:
            lhatG_runerror(L, "invalid option '%%%c' to 'lhat_pushfstring'", *(e + 1));
        }
        n += 2;
        fmt = e + 2;
    }
    lhatD_checkstack(L, 1);
    pushstr(L, fmt, strlen(fmt));
    if(n > 0) lhatV_concat(L, n + 1);
    return svalue(L->top - 1);
}


const char *lhatO_pushfstring(lhat_State *L, const char *fmt, ...)
{
    const char *msg;
    va_list argp;
    va_start(argp, fmt);
    msg = lhatO_pushvfstring(L, fmt, argp);
    va_end(argp);
    return msg;
}


// number of chars of a literal string without the ending \0
#define LL(x)	(sizeof(x)/sizeof(char) - 1)

#define RETS	"..."
#define PRE	"[string \""
#define POS	"\"]"

#define addstr(a,b,l)	( memcpy(a,b,(l) * sizeof(char)), a += (l) )

void lhatO_chunkid(char *out, const char *source, size_t bufflen)
{
    size_t l = strlen(source);
    if(*source == '=') {  // 'literal' source
        if(l <= bufflen)  // small enough?
            memcpy(out, source + 1, l * sizeof(char));
        else {  // truncate it
            addstr(out, source + 1, bufflen - 1);
            *out = '\0';
        }
    }
    else if(*source == '@') {  // file name
        if(l <= bufflen)  // small enough?
            memcpy(out, source + 1, l * sizeof(char));
        else {  // add '...' before rest of name
            addstr(out, RETS, LL(RETS));
            bufflen -= LL(RETS);
            memcpy(out, source + 1 + l - bufflen, bufflen * sizeof(char));
        }
    }
    else {  // string; format as [string "source"]
        const char *nl = strchr(source, '\n');  // find first new line (if any)
        addstr(out, PRE, LL(PRE));  // add prefix
        bufflen -= LL(PRE RETS POS) + 1;  // save space for prefix+suffix+'\0'
        if(l < bufflen && nl == NULL) {  // small one-line source?
            addstr(out, source, l);  // keep it
        }
        else {
            if(nl != NULL) l = nl - source;  // stop at first newline
            if(l > bufflen) l = bufflen;
            addstr(out, source, l);
            addstr(out, RETS, LL(RETS));
        }
        memcpy(out, POS, (LL(POS) + 1) * sizeof(char));
    }
}
