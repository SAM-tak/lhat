//
// Library for Table Manipulation
// See Copyright Notice in lhat.h
//

#define LHAT_LIB

#include "lprefix.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "lhat.h"

#include "lauxlib.h"
#include "lhatlib.h"

//
// Operations that an object must define to mimic a table
// (some functions only need some of them)
//
enum {
    TAB_R = 1,			        // read
    TAB_W = 2,		    	    // write
    TAB_L = 4,			        // length
    TAB_RW = (TAB_R | TAB_W),	// read/write
};


#define aux_getn(L,n,w)	(checktab(L, n, (w) | TAB_L), lhatL_len(L, n))


static int checkfield(lhat_State *L, const char *key, int n)
{
    lhat_pushstring(L, key);
    return (lhat_rawget(L, -n) != LHAT_TNIL);
}


//
// Check that 'arg' either is a table or can behave like one (that is,
// has a metatable with the required metamethods)
//
static void checktab(lhat_State *L, int arg, int what)
{
    if(lhat_type(L, arg) != LHAT_TTABLE) {  // is it not a table?
        int n = 1;  // number of elements to pop
        if(lhat_getmetatable(L, arg) &&  // must have metatable
            (!(what & TAB_R) || checkfield(L, "__index", ++n)) &&
            (!(what & TAB_W) || checkfield(L, "__newindex", ++n)) &&
            (!(what & TAB_L) || checkfield(L, "__len", ++n))) {
            lhat_pop(L, n);  // pop metatable and tested metamethods
        }
        else
            lhatL_checktype(L, arg, LHAT_TTABLE);  // force an error
    }
}


static int tinsert(lhat_State *L)
{
    lhat_Integer e = aux_getn(L, 1, TAB_RW) + 1;  // first empty element
    lhat_Integer pos;  // where to insert new element
    switch(lhat_gettop(L)) {
    case 2: {  // called with only 2 arguments
        pos = e;  // insert new element at the end
        break;
    }
    case 3: {
        lhat_Integer i;
        pos = lhatL_checkinteger(L, 2);  // 2nd argument is the position
        lhatL_argcheck(L, 1 <= pos && pos <= e, 2, "position out of bounds");
        for(i = e; i > pos; i--) {  // move up elements
            lhat_geti(L, 1, i - 1);
            lhat_seti(L, 1, i);  // t[i] = t[i - 1]
        }
        break;
    }
    default:
        return lhatL_error(L, "wrong number of arguments to 'insert'");
    }
    lhat_seti(L, 1, pos);  // t[pos] = v
    return 0;
}


static int tremove(lhat_State *L)
{
    lhat_Integer size = aux_getn(L, 1, TAB_RW);
    lhat_Integer pos = lhatL_optinteger(L, 2, size);
    if(pos != size)  // validate 'pos' if given
        lhatL_argcheck(L, 1 <= pos && pos <= size + 1, 1, "position out of bounds");
    lhat_geti(L, 1, pos);  // result = t[pos]
    for(; pos < size; pos++) {
        lhat_geti(L, 1, pos + 1);
        lhat_seti(L, 1, pos);  // t[pos] = t[pos + 1]
    }
    lhat_pushnil(L);
    lhat_seti(L, 1, pos);  // t[pos] = nil
    return 1;
}


//
// Copy elements (1[f], ..., 1[e]) into (tt[t], tt[t+1], ...). Whenever
// possible, copy in increasing order, which is better for rehashing.
// "possible" means destination after original range, or smaller
// than origin, or copying to another table.
//
static int tmove(lhat_State *L)
{
    lhat_Integer f = lhatL_checkinteger(L, 2);
    lhat_Integer e = lhatL_checkinteger(L, 3);
    lhat_Integer t = lhatL_checkinteger(L, 4);
    int tt = !lhat_isnoneornil(L, 5) ? 5 : 1;  // destination table
    checktab(L, 1, TAB_R);
    checktab(L, tt, TAB_W);
    if(e >= f) {  // otherwise, nothing to move
        lhat_Integer n, i;
        lhatL_argcheck(L, f > 0 || e < LHAT_MAXINTEGER + f, 3, "too many elements to move");
        n = e - f + 1;  // number of elements to move
        lhatL_argcheck(L, t <= LHAT_MAXINTEGER - n + 1, 4, "destination wrap around");
        if(t > e || t <= f || (tt != 1 && !lhat_compare(L, 1, tt, LHAT_OPEQ))) {
            for(i = 0; i < n; i++) {
                lhat_geti(L, 1, f + i);
                lhat_seti(L, tt, t + i);
            }
        }
        else {
            for(i = n - 1; i >= 0; i--) {
                lhat_geti(L, 1, f + i);
                lhat_seti(L, tt, t + i);
            }
        }
    }
    lhat_pushvalue(L, tt);  // return destination table
    return 1;
}


static void addfield(lhat_State *L, lhatL_Buffer *b, lhat_Integer i)
{
    lhat_geti(L, 1, i);
    if(!lhat_isstring(L, -1))
        lhatL_error(L, "invalid value (%s) at index %d in table for 'concat'", lhatL_typename(L, -1), i);
    lhatL_addvalue(b);
}


static int tconcat(lhat_State *L)
{
    lhat_Integer last = aux_getn(L, 1, TAB_R);
    size_t lsep;
    const char *sep = lhatL_optlstring(L, 2, "", &lsep);
    lhat_Integer i = lhatL_optinteger(L, 3, 1);
    last = lhatL_optinteger(L, 4, last);
    lhatL_Buffer b;
    lhatL_buffinit(L, &b);
    for(; i < last; i++) {
        addfield(L, &b, i);
        lhatL_addlstring(&b, sep, lsep);
    }
    if(i == last)  // add last value (if interval was not empty)
        addfield(L, &b, i);
    lhatL_pushresult(&b);
    return 1;
}


//
// {======================================================
// Pack/unpack
// =======================================================
//

static int pack(lhat_State *L)
{
    int i;
    int n = lhat_gettop(L);  // number of elements to pack
    lhat_createtable(L, n, 1);  // create result table
    lhat_insert(L, 1);  // put it at index 1
    for(i = n; i >= 1; i--)  // assign elements
        lhat_seti(L, 1, i);
    lhat_pushinteger(L, n);
    lhat_setfield(L, 1, "n");  // t.n = number of elements
    return 1;  // return table
}


static int unpack(lhat_State *L)
{
    lhat_Unsigned n;
    lhat_Integer i = lhatL_optinteger(L, 2, 1);
    lhat_Integer e = lhatL_opt(L, lhatL_checkinteger, 3, lhatL_len(L, 1));
    if(i > e) return 0;  // empty range
    n = (lhat_Unsigned)e - i;  // number of elements minus 1 (avoid overflows)
    if(n >= (unsigned int)INT_MAX || !lhat_checkstack(L, (int)(++n)))
        return lhatL_error(L, "too many results to unpack");
    for(; i < e; i++) {  // push arg[i..e - 1] (to avoid overflows)
        lhat_geti(L, 1, i);
    }
    lhat_geti(L, 1, e);  // push last element
    return (int)n;
}

// }======================================================



//
// {======================================================
// Quicksort
// (based on 'Algorithms in MODULA-3', Robert Sedgewick;
//  Addison-Wesley, 1993.)
// =======================================================
//


// type for array indices
typedef unsigned int IdxT;


//
// Produce a "random" 'unsigned int' to randomize pivot choice. This
// macro is used only when 'sort' detects a big imbalance in the result
// of a partition. (If you don't want/need this "randomness", ~0 is a
// good choice.)
//
#if !defined(l_randomizePivot)
# include <time.h>
// size of 'e' measured in number of 'unsigned int's
# define sof(e)		(sizeof(e) / sizeof(unsigned int))
//
// Use 'time' and 'clock' as sources of "randomness". Because we don't
// know the types 'clock_t' and 'time_t', we cannot cast them to
// anything without risking overflows. A safe way to use their values
// is to copy them to an array of a known type and use the array values.
//
static unsigned int l_randomizePivot(void)
{
    clock_t c = clock();
    time_t t = time(NULL);
    unsigned int buff[sof(c) + sof(t)];
    unsigned int i, rnd = 0;
    memcpy(buff, &c, sof(c) * sizeof(unsigned int));
    memcpy(buff + sof(c), &t, sof(t) * sizeof(unsigned int));
    for(i = 0; i < sof(buff); i++)
        rnd += buff[i];
    return rnd;
}
#endif

// arrays larger than 'RANLIMIT' may use randomized pivots
#define RANLIMIT	100u


static void set2(lhat_State *L, IdxT i, IdxT j)
{
    lhat_seti(L, 1, i);
    lhat_seti(L, 1, j);
}


//
// Return true iff value at stack index 'a' is less than the value at
// index 'b' (according to the order of the sort).
//
static int sort_comp(lhat_State *L, int a, int b)
{
    if(lhat_isnil(L, 2))  // no function?
        return lhat_compare(L, a, b, LHAT_OPLT);  // a < b
    else {  // function
        int res;
        lhat_pushvalue(L, 2);    // push function
        lhat_pushvalue(L, a - 1);  // -1 to compensate function
        lhat_pushvalue(L, b - 2);  // -2 to compensate function and 'a'
        lhat_call(L, 2, 1);      // call function
        res = lhat_toboolean(L, -1);  // get result
        lhat_pop(L, 1);          // pop result
        return res;
    }
}


//
// Does the partition: Pivot P is at the top of the stack.
// precondition: a[lo] <= P == a[up-1] <= a[up],
// so it only needs to do the partition from lo + 1 to up - 2.
// Pos-condition: a[lo .. i - 1] <= a[i] == P <= a[i + 1 .. up]
// returns 'i'.
//
static IdxT partition(lhat_State *L, IdxT lo, IdxT up)
{
    IdxT i = lo;  // will be incremented before first use
    IdxT j = up - 1;  // will be decremented before first use
    // loop invariant: a[lo .. i] <= P <= a[j .. up]
    for(;;) {
        // next loop: repeat ++i while a[i] < P
        while(lhat_geti(L, 1, ++i), sort_comp(L, -1, -2)) {
            if(i == up - 1)  // a[i] < P  but a[up - 1] == P  ??
                lhatL_error(L, "invalid order function for sorting");
            lhat_pop(L, 1);  // remove a[i]
        }
        // after the loop, a[i] >= P and a[lo .. i - 1] < P
        // next loop: repeat --j while P < a[j]
        while(lhat_geti(L, 1, --j), sort_comp(L, -3, -1)) {
            if(j < i)  // j < i  but  a[j] > P ??
                lhatL_error(L, "invalid order function for sorting");
            lhat_pop(L, 1);  // remove a[j]
        }
        // after the loop, a[j] <= P and a[j + 1 .. up] >= P
        if(j < i) {  // no elements out of place?
                     // a[lo .. i - 1] <= P <= a[j + 1 .. i .. up]
            lhat_pop(L, 1);  // pop a[j]
                             // swap pivot (a[up - 1]) with a[i] to satisfy pos-condition
            set2(L, up - 1, i);
            return i;
        }
        // otherwise, swap a[i] - a[j] to restore invariant and repeat
        set2(L, i, j);
    }
}


//
// Choose an element in the middle (2nd-3th quarters) of [lo,up]
// "randomized" by 'rnd'
//
static IdxT choosePivot(IdxT lo, IdxT up, unsigned int rnd)
{
    IdxT r4 = (up - lo) / 4;  // range/4
    IdxT p = rnd % (r4 * 2) + (lo + r4);
    lhat_assert(lo + r4 <= p && p <= up - r4);
    return p;
}


//
// QuickSort algorithm (recursive function)
//
static void auxsort(lhat_State *L, IdxT lo, IdxT up, unsigned int rnd)
{
    while(lo < up) {  // loop for tail recursion
        IdxT p;  // Pivot index
        IdxT n;  // to be used later
                 // sort elements 'lo', 'p', and 'up'
        lhat_geti(L, 1, lo);
        lhat_geti(L, 1, up);
        if(sort_comp(L, -1, -2))  // a[up] < a[lo]?
            set2(L, lo, up);  // swap a[lo] - a[up]
        else
            lhat_pop(L, 2);  // remove both values
        if(up - lo == 1)  // only 2 elements?
            return;  // already sorted
        if(up - lo < RANLIMIT || rnd == 0)  // small interval or no randomize?
            p = (lo + up) / 2;  // middle element is a good pivot
        else  // for larger intervals, it is worth a random pivot
            p = choosePivot(lo, up, rnd);
        lhat_geti(L, 1, p);
        lhat_geti(L, 1, lo);
        if(sort_comp(L, -2, -1))  // a[p] < a[lo]?
            set2(L, p, lo);  // swap a[p] - a[lo]
        else {
            lhat_pop(L, 1);  // remove a[lo]
            lhat_geti(L, 1, up);
            if(sort_comp(L, -1, -2))  // a[up] < a[p]?
                set2(L, p, up);  // swap a[up] - a[p]
            else
                lhat_pop(L, 2);
        }
        if(up - lo == 2)  // only 3 elements?
            return;  // already sorted
        lhat_geti(L, 1, p);  // get middle element (Pivot)
        lhat_pushvalue(L, -1);  // push Pivot
        lhat_geti(L, 1, up - 1);  // push a[up - 1]
        set2(L, p, up - 1);  // swap Pivot (a[p]) with a[up - 1]
        p = partition(L, lo, up);
        // a[lo .. p - 1] <= a[p] == P <= a[p + 1 .. up]
        if(p - lo < up - p) {  // lower interval is smaller?
            auxsort(L, lo, p - 1, rnd);  // call recursively for lower interval
            n = p - lo;  // size of smaller interval
            lo = p + 1;  // tail call for [p + 1 .. up] (upper interval)
        }
        else {
            auxsort(L, p + 1, up, rnd);  // call recursively for upper interval
            n = up - p;  // size of smaller interval
            up = p - 1;  // tail call for [lo .. p - 1]  (lower interval)
        }
        if((up - lo) / 128 > n) // partition too imbalanced?
            rnd = l_randomizePivot();  // try a new randomization
    }  // tail call auxsort(L, lo, up, rnd)
}


static int sort(lhat_State *L)
{
    lhat_Integer n = aux_getn(L, 1, TAB_RW);
    if(n > 1) {  // non-trivial interval?
        lhatL_argcheck(L, n < INT_MAX, 1, "array too big");
        if(!lhat_isnoneornil(L, 2))  // is there a 2nd argument?
            lhatL_checktype(L, 2, LHAT_TFUNCTION);  // must be a function
        lhat_settop(L, 2);  // make sure there are two arguments
        auxsort(L, 1, (IdxT)n, 0);
    }
    return 0;
}

// }======================================================


static const lhatL_Reg tab_funcs[] = {
    { "concat", tconcat },
    { "insert", tinsert },
    { "pack", pack },
    { "unpack", unpack },
    { "remove", tremove },
    { "move", tmove },
    { "sort", sort },
    { NULL, NULL }
};


LHATMOD_API int lhatopen_table(lhat_State *L)
{
    lhatL_newlib(L, tab_funcs);
    return 1;
}
