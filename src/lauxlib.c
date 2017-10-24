//
// Auxiliary functions for building L^ libraries
// See Copyright Notice in lhat.h
//

#define lauxlib_c
#define LHAT_LIB

#include "lprefix.h"


#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


//
// This file uses only the official API of Lhat.
// Any function declared here could be written as an application function.
//

#include "lhat.h"

#include "lauxlib.h"


//
// {======================================================
// Traceback
// =======================================================
//


#define LEVELS1	10 // size of the first part of the stack
#define LEVELS2	11 // size of the second part of the stack

//
// search for 'objidx' in table at index -1.
// return 1 + string at top if find a good name.
//
static int findfield (lhat_State *L, int objidx, int level) {
  if (level == 0 || !lhat_istable(L, -1))
    return 0;  // not found
  lhat_pushnil(L);  // start 'next' loop
  while (lhat_next(L, -2)) {  // for each pair in table
    if (lhat_type(L, -2) == LHAT_TSTRING) {  // ignore non-string keys
      if (lhat_rawequal(L, objidx, -1)) {  // found object?
        lhat_pop(L, 1);  // remove value (but keep name)
        return 1;
      }
      else if (findfield(L, objidx, level - 1)) {  // try recursively
        lhat_remove(L, -2);  // remove table (but keep name)
        lhat_pushliteral(L, ".");
        lhat_insert(L, -2);  // place '.' between the two names
        lhat_concat(L, 3);
        return 1;
      }
    }
    lhat_pop(L, 1);  // remove value
  }
  return 0;  // not found
}


//
// Search for a name for a function in all loaded modules
//
static int pushglobalfuncname (lhat_State *L, lhat_Debug *ar) {
  int top = lhat_gettop(L);
  lhat_getinfo(L, "f", ar);  // push function
  lhat_getfield(L, LHAT_REGISTRYINDEX, LHAT_LOADED_TABLE);
  if (findfield(L, top + 1, 2)) {
    const char *name = lhat_tostring(L, -1);
    if (strncmp(name, "_G.", 3) == 0) {  // name start with '_G.'?
      lhat_pushstring(L, name + 3);  // push name without prefix
      lhat_remove(L, -2);  // remove original name
    }
    lhat_copy(L, -1, top + 1);  // move name to proper place
    lhat_pop(L, 2);  // remove pushed values
    return 1;
  }
  else {
    lhat_settop(L, top);  // remove function and global table
    return 0;
  }
}


static void pushfuncname (lhat_State *L, lhat_Debug *ar) {
  if (pushglobalfuncname(L, ar)) {  // try first a global name
    lhat_pushfstring(L, "function '%s'", lhat_tostring(L, -1));
    lhat_remove(L, -2);  // remove name
  }
  else if (*ar->namewhat != '\0')  // is there a name from code?
    lhat_pushfstring(L, "%s '%s'", ar->namewhat, ar->name);  // use it
  else if (*ar->what == 'm')  // main?
      lhat_pushliteral(L, "main chunk");
  else if (*ar->what != 'C')  // for Lhat functions, use <file:line>
    lhat_pushfstring(L, "function <%s:%d>", ar->short_src, ar->linedefined);
  else  // nothing left...
    lhat_pushliteral(L, "?");
}


static int lastlevel (lhat_State *L) {
  lhat_Debug ar;
  int li = 1, le = 1;
  // find an upper bound
  while (lhat_getstack(L, le, &ar)) { li = le; le *= 2; }
  // do a binary search
  while (li < le) {
    int m = (li + le)/2;
    if (lhat_getstack(L, m, &ar)) li = m + 1;
    else le = m;
  }
  return le - 1;
}


LHATLIB_API void lhatL_traceback (lhat_State *L, lhat_State *L1,
                                const char *msg, int level) {
  lhat_Debug ar;
  int top = lhat_gettop(L);
  int last = lastlevel(L1);
  int n1 = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
  if (msg)
    lhat_pushfstring(L, "%s\n", msg);
  lhatL_checkstack(L, 10, NULL);
  lhat_pushliteral(L, "stack traceback:");
  while (lhat_getstack(L1, level++, &ar)) {
    if (n1-- == 0) {  // too many levels?
      lhat_pushliteral(L, "\n\t...");  // add a '...'
      level = last - LEVELS2 + 1;  // and skip to last ones
    }
    else {
      lhat_getinfo(L1, "Slnt", &ar);
      lhat_pushfstring(L, "\n\t%s:", ar.short_src);
      if (ar.currentline > 0)
        lhat_pushfstring(L, "%d:", ar.currentline);
      lhat_pushliteral(L, " in ");
      pushfuncname(L, &ar);
      if (ar.istailcall)
        lhat_pushliteral(L, "\n\t(...tail calls...)");
      lhat_concat(L, lhat_gettop(L) - top);
    }
  }
  lhat_concat(L, lhat_gettop(L) - top);
}

// }======================================================


//
// {======================================================
// Error-report functions
// =======================================================
//

LHATLIB_API int lhatL_argerror (lhat_State *L, int arg, const char *extramsg) {
  lhat_Debug ar;
  if (!lhat_getstack(L, 0, &ar))  // no stack frame?
    return lhatL_error(L, "bad argument #%d (%s)", arg, extramsg);
  lhat_getinfo(L, "n", &ar);
  if (strcmp(ar.namewhat, "method") == 0) {
    arg--;  // do not count 'self'
    if (arg == 0)  // error is in the self argument itself?
      return lhatL_error(L, "calling '%s' on bad self (%s)",
                           ar.name, extramsg);
  }
  if (ar.name == NULL)
    ar.name = (pushglobalfuncname(L, &ar)) ? lhat_tostring(L, -1) : "?";
  return lhatL_error(L, "bad argument #%d to '%s' (%s)",
                        arg, ar.name, extramsg);
}


static int typeerror (lhat_State *L, int arg, const char *tname) {
  const char *msg;
  const char *typearg;  // name for the type of the actual argument
  if (lhatL_getmetafield(L, arg, "__name") == LHAT_TSTRING)
    typearg = lhat_tostring(L, -1);  // use the given type name
  else if (lhat_type(L, arg) == LHAT_TLIGHTUSERDATA)
    typearg = "light userdata";  // special name for messages
  else
    typearg = lhatL_typename(L, arg);  // standard name
  msg = lhat_pushfstring(L, "%s expected, got %s", tname, typearg);
  return lhatL_argerror(L, arg, msg);
}


static void tag_error (lhat_State *L, int arg, int tag) {
  typeerror(L, arg, lhat_typename(L, tag));
}


//
// The use of 'lhat_pushfstring' ensures this function does not
// need reserved stack space when called.
//
LHATLIB_API void lhatL_where (lhat_State *L, int level) {
  lhat_Debug ar;
  if (lhat_getstack(L, level, &ar)) {  // check function at level
    lhat_getinfo(L, "Sl", &ar);  // get info about it
    if (ar.currentline > 0) {  // is there info?
      lhat_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
      return;
    }
  }
  lhat_pushfstring(L, "");  // else, no information available...
}


//
// Again, the use of 'lhat_pushvfstring' ensures this function does
// not need reserved stack space when called. (At worst, it generates
// an error with "stack overflow" instead of the given message.)
//
LHATLIB_API int lhatL_error (lhat_State *L, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  lhatL_where(L, 1);
  lhat_pushvfstring(L, fmt, argp);
  va_end(argp);
  lhat_concat(L, 2);
  return lhat_error(L);
}


LHATLIB_API int lhatL_fileresult (lhat_State *L, int stat, const char *fname) {
  int en = errno;  // calls to Lhat API may change this value
  if (stat) {
    lhat_pushboolean(L, 1);
    return 1;
  }
  else {
    lhat_pushnil(L);
    if (fname)
      lhat_pushfstring(L, "%s: %s", fname, strerror(en));
    else
      lhat_pushstring(L, strerror(en));
    lhat_pushinteger(L, en);
    return 3;
  }
}


#if !defined(l_inspectstat)	// {

#if defined(LHAT_USE_POSIX)

#include <sys/wait.h>

//
// use appropriate macros to interpret 'pclose' return status
//
#define l_inspectstat(stat,what)  \
   if (WIFEXITED(stat)) { stat = WEXITSTATUS(stat); } \
   else if (WIFSIGNALED(stat)) { stat = WTERMSIG(stat); what = "signal"; }

#else

#define l_inspectstat(stat,what)  // no op

#endif

#endif				// }


LHATLIB_API int lhatL_execresult (lhat_State *L, int stat) {
  const char *what = "exit";  // type of termination
  if (stat == -1)  // error?
    return lhatL_fileresult(L, 0, NULL);
  else {
    l_inspectstat(stat, what);  // interpret result
    if (*what == 'e' && stat == 0)  // successful termination?
      lhat_pushboolean(L, 1);
    else
      lhat_pushnil(L);
    lhat_pushstring(L, what);
    lhat_pushinteger(L, stat);
    return 3;  // return true/nil,what,code
  }
}

// }======================================================


//
// {======================================================
// Userdata's metatable manipulation
// =======================================================
//

LHATLIB_API int lhatL_newmetatable (lhat_State *L, const char *tname) {
  if (lhatL_getmetatable(L, tname) != LHAT_TNIL)  // name already in use?
    return 0;  // leave previous value on top, but return 0
  lhat_pop(L, 1);
  lhat_createtable(L, 0, 2);  // create metatable
  lhat_pushstring(L, tname);
  lhat_setfield(L, -2, "__name");  // metatable.__name = tname
  lhat_pushvalue(L, -1);
  lhat_setfield(L, LHAT_REGISTRYINDEX, tname);  // registry.name = metatable
  return 1;
}


LHATLIB_API void lhatL_setmetatable (lhat_State *L, const char *tname) {
  lhatL_getmetatable(L, tname);
  lhat_setmetatable(L, -2);
}


LHATLIB_API void *lhatL_testudata (lhat_State *L, int ud, const char *tname) {
  void *p = lhat_touserdata(L, ud);
  if (p != NULL) {  // value is a userdata?
    if (lhat_getmetatable(L, ud)) {  // does it have a metatable?
      lhatL_getmetatable(L, tname);  // get correct metatable
      if (!lhat_rawequal(L, -1, -2))  // not the same?
        p = NULL;  // value is a userdata with wrong metatable
      lhat_pop(L, 2);  // remove both metatables
      return p;
    }
  }
  return NULL;  // value is not a userdata with a metatable
}


LHATLIB_API void *lhatL_checkudata (lhat_State *L, int ud, const char *tname) {
  void *p = lhatL_testudata(L, ud, tname);
  if (p == NULL) typeerror(L, ud, tname);
  return p;
}

// }======================================================


//
// {======================================================
// Argument check functions
// =======================================================
//

LHATLIB_API int lhatL_checkoption (lhat_State *L, int arg, const char *def,
                                 const char *const lst[]) {
  const char *name = (def) ? lhatL_optstring(L, arg, def) :
                             lhatL_checkstring(L, arg);
  int i;
  for (i=0; lst[i]; i++)
    if (strcmp(lst[i], name) == 0)
      return i;
  return lhatL_argerror(L, arg,
                       lhat_pushfstring(L, "invalid option '%s'", name));
}


//
// Ensures the stack has at least 'space' extra slots, raising an error
// if it cannot fulfill the request. (The error handling needs a few
// extra slots to format the error message. In case of an error without
// this extra space, Lhat will generate the same 'stack overflow' error,
// but without 'msg'.)
//
LHATLIB_API void lhatL_checkstack (lhat_State *L, int space, const char *msg) {
  if (!lhat_checkstack(L, space)) {
    if (msg)
      lhatL_error(L, "stack overflow (%s)", msg);
    else
      lhatL_error(L, "stack overflow");
  }
}


LHATLIB_API void lhatL_checktype (lhat_State *L, int arg, int t) {
  if (lhat_type(L, arg) != t)
    tag_error(L, arg, t);
}


LHATLIB_API void lhatL_checkany (lhat_State *L, int arg) {
  if (lhat_type(L, arg) == LHAT_TNONE)
    lhatL_argerror(L, arg, "value expected");
}


LHATLIB_API const char *lhatL_checklstring (lhat_State *L, int arg, size_t *len) {
  const char *s = lhat_tolstring(L, arg, len);
  if (!s) tag_error(L, arg, LHAT_TSTRING);
  return s;
}


LHATLIB_API const char *lhatL_optlstring (lhat_State *L, int arg,
                                        const char *def, size_t *len) {
  if (lhat_isnoneornil(L, arg)) {
    if (len)
      *len = (def ? strlen(def) : 0);
    return def;
  }
  else return lhatL_checklstring(L, arg, len);
}


LHATLIB_API lhat_Number lhatL_checknumber (lhat_State *L, int arg) {
  int isnum;
  lhat_Number d = lhat_tonumberx(L, arg, &isnum);
  if (!isnum)
    tag_error(L, arg, LHAT_TNUMBER);
  return d;
}


LHATLIB_API lhat_Number lhatL_optnumber (lhat_State *L, int arg, lhat_Number def) {
  return lhatL_opt(L, lhatL_checknumber, arg, def);
}


static void interror (lhat_State *L, int arg) {
  if (lhat_isnumber(L, arg))
    lhatL_argerror(L, arg, "number has no integer representation");
  else
    tag_error(L, arg, LHAT_TNUMBER);
}


LHATLIB_API lhat_Integer lhatL_checkinteger (lhat_State *L, int arg) {
  int isnum;
  lhat_Integer d = lhat_tointegerx(L, arg, &isnum);
  if (!isnum) {
    interror(L, arg);
  }
  return d;
}


LHATLIB_API lhat_Integer lhatL_optinteger (lhat_State *L, int arg,
                                                      lhat_Integer def) {
  return lhatL_opt(L, lhatL_checkinteger, arg, def);
}

// }======================================================


//
// {======================================================
// Generic Buffer manipulation
// =======================================================
//

// userdata to box arbitrary data
typedef struct UBox {
  void *box;
  size_t bsize;
} UBox;


static void *resizebox (lhat_State *L, int idx, size_t newsize) {
  void *ud;
  lhat_Alloc allocf = lhat_getallocf(L, &ud);
  UBox *box = (UBox *)lhat_touserdata(L, idx);
  void *temp = allocf(ud, box->box, box->bsize, newsize);
  if (temp == NULL && newsize > 0) {  // allocation error?
    resizebox(L, idx, 0);  // free buffer
    lhatL_error(L, "not enough memory for buffer allocation");
  }
  box->box = temp;
  box->bsize = newsize;
  return temp;
}


static int boxgc (lhat_State *L) {
  resizebox(L, 1, 0);
  return 0;
}


static void *newbox (lhat_State *L, size_t newsize) {
  UBox *box = (UBox *)lhat_newuserdata(L, sizeof(UBox));
  box->box = NULL;
  box->bsize = 0;
  if (lhatL_newmetatable(L, "LHATBOX")) {  // creating metatable?
    lhat_pushcfunction(L, boxgc);
    lhat_setfield(L, -2, "__gc");  // metatable.__gc = boxgc
  }
  lhat_setmetatable(L, -2);
  return resizebox(L, -1, newsize);
}


//
// check whether buffer is using a userdata on the stack as a temporary
// buffer
//
#define buffonstack(B)	((B)->b != (B)->initb)


//
// returns a pointer to a free area with at least 'sz' bytes
//
LHATLIB_API char *lhatL_prepbuffsize (lhatL_Buffer *B, size_t sz) {
  lhat_State *L = B->L;
  if (B->size - B->n < sz) {  // not enough space?
    char *newbuff;
    size_t newsize = B->size * 2;  // double buffer size
    if (newsize - B->n < sz)  // not big enough?
      newsize = B->n + sz;
    if (newsize < B->n || newsize - B->n < sz)
      lhatL_error(L, "buffer too large");
    // create larger buffer
    if (buffonstack(B))
      newbuff = (char *)resizebox(L, -1, newsize);
    else {  // no buffer yet
      newbuff = (char *)newbox(L, newsize);
      memcpy(newbuff, B->b, B->n * sizeof(char));  // copy original content
    }
    B->b = newbuff;
    B->size = newsize;
  }
  return &B->b[B->n];
}


LHATLIB_API void lhatL_addlstring (lhatL_Buffer *B, const char *s, size_t l) {
  if (l > 0) {  // avoid 'memcpy' when 's' can be NULL
    char *b = lhatL_prepbuffsize(B, l);
    memcpy(b, s, l * sizeof(char));
    lhatL_addsize(B, l);
  }
}


LHATLIB_API void lhatL_addstring (lhatL_Buffer *B, const char *s) {
  lhatL_addlstring(B, s, strlen(s));
}


LHATLIB_API void lhatL_pushresult (lhatL_Buffer *B) {
  lhat_State *L = B->L;
  lhat_pushlstring(L, B->b, B->n);
  if (buffonstack(B)) {
    resizebox(L, -2, 0);  // delete old buffer
    lhat_remove(L, -2);  // remove its header from the stack
  }
}


LHATLIB_API void lhatL_pushresultsize (lhatL_Buffer *B, size_t sz) {
  lhatL_addsize(B, sz);
  lhatL_pushresult(B);
}


LHATLIB_API void lhatL_addvalue (lhatL_Buffer *B) {
  lhat_State *L = B->L;
  size_t l;
  const char *s = lhat_tolstring(L, -1, &l);
  if (buffonstack(B))
    lhat_insert(L, -2);  // put value below buffer
  lhatL_addlstring(B, s, l);
  lhat_remove(L, (buffonstack(B)) ? -2 : -1);  // remove value
}


LHATLIB_API void lhatL_buffinit (lhat_State *L, lhatL_Buffer *B) {
  B->L = L;
  B->b = B->initb;
  B->n = 0;
  B->size = LHATL_BUFFERSIZE;
}


LHATLIB_API char *lhatL_buffinitsize (lhat_State *L, lhatL_Buffer *B, size_t sz) {
  lhatL_buffinit(L, B);
  return lhatL_prepbuffsize(B, sz);
}

// }======================================================


//
// {======================================================
// Reference system
// =======================================================
//

// index of free-list header
#define freelist	0


LHATLIB_API int lhatL_ref (lhat_State *L, int t) {
  int ref;
  if (lhat_isnil(L, -1)) {
    lhat_pop(L, 1);  // remove from stack
    return LHAT_REFNIL;  // 'nil' has a unique fixed reference
  }
  t = lhat_absindex(L, t);
  lhat_rawgeti(L, t, freelist);  // get first free element
  ref = (int)lhat_tointeger(L, -1);  // ref = t[freelist]
  lhat_pop(L, 1);  // remove it from stack
  if (ref != 0) {  // any free element?
    lhat_rawgeti(L, t, ref);  // remove it from list
    lhat_rawseti(L, t, freelist);  // (t[freelist] = t[ref])
  }
  else  // no free elements
    ref = (int)lhat_rawlen(L, t) + 1;  // get a new reference
  lhat_rawseti(L, t, ref);
  return ref;
}


LHATLIB_API void lhatL_unref (lhat_State *L, int t, int ref) {
  if (ref >= 0) {
    t = lhat_absindex(L, t);
    lhat_rawgeti(L, t, freelist);
    lhat_rawseti(L, t, ref);  // t[ref] = t[freelist]
    lhat_pushinteger(L, ref);
    lhat_rawseti(L, t, freelist);  // t[freelist] = ref
  }
}

// }======================================================


//
// {======================================================
// Load functions
// =======================================================
//

typedef struct LoadF {
  int n;  // number of pre-read characters
  FILE *f;  // file being read
  char buff[BUFSIZ];  // area for reading file
} LoadF;


static const char *getF (lhat_State *L, void *ud, size_t *size) {
  LoadF *lf = (LoadF *)ud;
  (void)L;  // not used
  if (lf->n > 0) {  // are there pre-read characters to be read?
    *size = lf->n;  // return them (chars already in buffer)
    lf->n = 0;  // no more pre-read characters
  }
  else {  // read a block from file
    // 'fread' can return > 0 *and* set the EOF flag. If next call to
    //   'getF' called 'fread', it might still wait for user input.
    //   The next check avoids this problem.
    if (feof(lf->f)) return NULL;
    *size = fread(lf->buff, 1, sizeof(lf->buff), lf->f);  // read block
  }
  return lf->buff;
}


static int errfile (lhat_State *L, const char *what, int fnameindex) {
  const char *serr = strerror(errno);
  const char *filename = lhat_tostring(L, fnameindex) + 1;
  lhat_pushfstring(L, "cannot %s %s: %s", what, filename, serr);
  lhat_remove(L, fnameindex);
  return LHAT_ERRFILE;
}


static int skipBOM (LoadF *lf) {
  const char *p = "\xEF\xBB\xBF";  // UTF-8 BOM mark
  int c;
  lf->n = 0;
  do {
    c = getc(lf->f);
    if (c == EOF || c != *(const unsigned char *)p++) return c;
    lf->buff[lf->n++] = c;  // to be read by the parser
  } while (*p != '\0');
  lf->n = 0;  // prefix matched; discard it
  return getc(lf->f);  // return next character
}


//
// reads the first character of file 'f' and skips an optional BOM mark
// in its beginning plus its first line if it starts with '#'. Returns
// true if it skipped the first line.  In any case, '*cp' has the
// first "valid" character of the file (after the optional BOM and
// a first-line comment).
//
static int skipcomment (LoadF *lf, int *cp) {
  int c = *cp = skipBOM(lf);
  if (c == '#') {  // first line is a comment (Unix exec. file)?
    do {  // skip first line
      c = getc(lf->f);
    } while (c != EOF && c != '\n');
    *cp = getc(lf->f);  // skip end-of-line, if present
    return 1;  // there was a comment
  }
  else return 0;  // no comment
}


LHATLIB_API int lhatL_loadfilex (lhat_State *L, const char *filename,
                                             const char *mode) {
  LoadF lf;
  int status, readstatus;
  int c;
  int fnameindex = lhat_gettop(L) + 1;  // index of filename on the stack
  if (filename == NULL) {
    lhat_pushliteral(L, "=stdin");
    lf.f = stdin;
  }
  else {
    lhat_pushfstring(L, "@%s", filename);
    lf.f = fopen(filename, "r");
    if (lf.f == NULL) return errfile(L, "open", fnameindex);
  }
  if (skipcomment(&lf, &c))  // read initial portion
    lf.buff[lf.n++] = '\n';  // add line to correct line numbers
  if (c == LHAT_SIGNATURE[0] && filename) {  // binary file?
    lf.f = freopen(filename, "rb", lf.f);  // reopen in binary mode
    if (lf.f == NULL) return errfile(L, "reopen", fnameindex);
    skipcomment(&lf, &c);  // re-read initial portion
  }
  if (c != EOF)
    lf.buff[lf.n++] = c;  // 'c' is the first character of the stream
  status = lhat_load(L, getF, &lf, lhat_tostring(L, -1), mode);
  readstatus = ferror(lf.f);
  if (filename) fclose(lf.f);  // close file (even in case of errors)
  if (readstatus) {
    lhat_settop(L, fnameindex);  // ignore results from 'lhat_load'
    return errfile(L, "read", fnameindex);
  }
  lhat_remove(L, fnameindex);
  return status;
}


typedef struct LoadS {
  const char *s;
  size_t size;
} LoadS;


static const char *getS (lhat_State *L, void *ud, size_t *size) {
  LoadS *ls = (LoadS *)ud;
  (void)L;  // not used
  if (ls->size == 0) return NULL;
  *size = ls->size;
  ls->size = 0;
  return ls->s;
}


LHATLIB_API int lhatL_loadbufferx (lhat_State *L, const char *buff, size_t size,
                                 const char *name, const char *mode) {
  LoadS ls;
  ls.s = buff;
  ls.size = size;
  return lhat_load(L, getS, &ls, name, mode);
}


LHATLIB_API int lhatL_loadstring (lhat_State *L, const char *s) {
  return lhatL_loadbuffer(L, s, strlen(s), s);
}

// }======================================================



LHATLIB_API int lhatL_getmetafield (lhat_State *L, int obj, const char *event) {
  if (!lhat_getmetatable(L, obj))  // no metatable?
    return LHAT_TNIL;
  else {
    int tt;
    lhat_pushstring(L, event);
    tt = lhat_rawget(L, -2);
    if (tt == LHAT_TNIL)  // is metafield nil?
      lhat_pop(L, 2);  // remove metatable and metafield
    else
      lhat_remove(L, -2);  // remove only metatable
    return tt;  // return metafield type
  }
}


LHATLIB_API int lhatL_callmeta (lhat_State *L, int obj, const char *event) {
  obj = lhat_absindex(L, obj);
  if (lhatL_getmetafield(L, obj, event) == LHAT_TNIL)  // no metafield?
    return 0;
  lhat_pushvalue(L, obj);
  lhat_call(L, 1, 1);
  return 1;
}


LHATLIB_API lhat_Integer lhatL_len (lhat_State *L, int idx) {
  lhat_Integer l;
  int isnum;
  lhat_len(L, idx);
  l = lhat_tointegerx(L, -1, &isnum);
  if (!isnum)
    lhatL_error(L, "object length is not an integer");
  lhat_pop(L, 1);  // remove object
  return l;
}


LHATLIB_API const char *lhatL_tolstring (lhat_State *L, int idx, size_t *len) {
  if (lhatL_callmeta(L, idx, "__tostring")) {  // metafield?
    if (!lhat_isstring(L, -1))
      lhatL_error(L, "'__tostring' must return a string");
  }
  else {
    switch (lhat_type(L, idx)) {
      case LHAT_TNUMBER: {
        if (lhat_isinteger(L, idx))
          lhat_pushfstring(L, "%I", (LHATI_UACINT)lhat_tointeger(L, idx));
        else
          lhat_pushfstring(L, "%f", (LHATI_UACNUMBER)lhat_tonumber(L, idx));
        break;
      }
      case LHAT_TSTRING:
        lhat_pushvalue(L, idx);
        break;
      case LHAT_TBOOLEAN:
        lhat_pushstring(L, (lhat_toboolean(L, idx) ? "true" : "false"));
        break;
      case LHAT_TNIL:
        lhat_pushliteral(L, "nil");
        break;
      default: {
        int tt = lhatL_getmetafield(L, idx, "__name");  // try name
        const char *kind = (tt == LHAT_TSTRING) ? lhat_tostring(L, -1) :
                                                 lhatL_typename(L, idx);
        lhat_pushfstring(L, "%s: %p", kind, lhat_topointer(L, idx));
        if (tt != LHAT_TNIL)
          lhat_remove(L, -2);  // remove '__name'
        break;
      }
    }
  }
  return lhat_tolstring(L, -1, len);
}


//
// {======================================================
// Compatibility with 5.1 module functions
// =======================================================
//
#if defined(LHAT_COMPAT_MODULE)

static const char *lhatL_findtable (lhat_State *L, int idx,
                                   const char *fname, int szhint) {
  const char *e;
  if (idx) lhat_pushvalue(L, idx);
  do {
    e = strchr(fname, '.');
    if (e == NULL) e = fname + strlen(fname);
    lhat_pushlstring(L, fname, e - fname);
    if (lhat_rawget(L, -2) == LHAT_TNIL) {  // no such field?
      lhat_pop(L, 1);  // remove this nil
      lhat_createtable(L, 0, (*e == '.' ? 1 : szhint)); // new table for field
      lhat_pushlstring(L, fname, e - fname);
      lhat_pushvalue(L, -2);
      lhat_settable(L, -4);  // set new table into field
    }
    else if (!lhat_istable(L, -1)) {  // field has a non-table value?
      lhat_pop(L, 2);  // remove table and value
      return fname;  // return problematic part of the name
    }
    lhat_remove(L, -2);  // remove previous table
    fname = e + 1;
  } while (*e == '.');
  return NULL;
}


//
// Count number of elements in a lhatL_Reg list.
//
static int libsize (const lhatL_Reg *l) {
  int size = 0;
  for (; l && l->name; l++) size++;
  return size;
}


//
// Find or create a module table with a given name. The function
// first looks at the LOADED table and, if that fails, try a
// global variable with that name. In any case, leaves on the stack
// the module table.
//
LHATLIB_API void lhatL_pushmodule (lhat_State *L, const char *modname,
                                 int sizehint) {
  lhatL_findtable(L, LHAT_REGISTRYINDEX, LHAT_LOADED_TABLE, 1);
  if (lhat_getfield(L, -1, modname) != LHAT_TTABLE) {  // no LOADED[modname]?
    lhat_pop(L, 1);  // remove previous result
    // try global variable (and create one if it does not exist)
    lhat_pushglobaltable(L);
    if (lhatL_findtable(L, 0, modname, sizehint) != NULL)
      lhatL_error(L, "name conflict for module '%s'", modname);
    lhat_pushvalue(L, -1);
    lhat_setfield(L, -3, modname);  // LOADED[modname] = new table
  }
  lhat_remove(L, -2);  // remove LOADED table
}


LHATLIB_API void lhatL_openlib (lhat_State *L, const char *libname,
                               const lhatL_Reg *l, int nup) {
  lhatL_checkversion(L);
  if (libname) {
    lhatL_pushmodule(L, libname, libsize(l));  // get/create library table
    lhat_insert(L, -(nup + 1));  // move library table to below upvalues
  }
  if (l)
    lhatL_setfuncs(L, l, nup);
  else
    lhat_pop(L, nup);  // remove upvalues
}

#endif
// }======================================================

//
// set functions from list 'l' into table at top - 'nup'; each
// function gets the 'nup' elements at the top as upvalues.
// Returns with only the table at the stack.
//
LHATLIB_API void lhatL_setfuncs (lhat_State *L, const lhatL_Reg *l, int nup) {
  lhatL_checkstack(L, nup, "too many upvalues");
  for (; l->name != NULL; l++) {  // fill the table with given functions
    int i;
    for (i = 0; i < nup; i++)  // copy upvalues to the top
      lhat_pushvalue(L, -nup);
    lhat_pushcclosure(L, l->func, nup);  // closure with those upvalues
    lhat_setfield(L, -(nup + 2), l->name);
  }
  lhat_pop(L, nup);  // remove upvalues
}


//
// ensure that stack[idx][fname] has a table and push that table
// into the stack
//
LHATLIB_API int lhatL_getsubtable (lhat_State *L, int idx, const char *fname) {
  if (lhat_getfield(L, idx, fname) == LHAT_TTABLE)
    return 1;  // table already there
  else {
    lhat_pop(L, 1);  // remove previous result
    idx = lhat_absindex(L, idx);
    lhat_newtable(L);
    lhat_pushvalue(L, -1);  // copy to be left at top
    lhat_setfield(L, idx, fname);  // assign new table to field
    return 0;  // false, because did not find table there
  }
}


//
// Stripped-down 'require': After checking "loaded" table, calls 'openf'
// to open a module, registers the result in 'package.loaded' table and,
// if 'glb' is true, also registers the result in the global table.
// Leaves resulting module on the top.
//
LHATLIB_API void lhatL_requiref (lhat_State *L, const char *modname,
                               lhat_CFunction openf, int glb) {
  lhatL_getsubtable(L, LHAT_REGISTRYINDEX, LHAT_LOADED_TABLE);
  lhat_getfield(L, -1, modname);  // LOADED[modname]
  if (!lhat_toboolean(L, -1)) {  // package not already loaded?
    lhat_pop(L, 1);  // remove field
    lhat_pushcfunction(L, openf);
    lhat_pushstring(L, modname);  // argument to open function
    lhat_call(L, 1, 1);  // call 'openf' to open module
    lhat_pushvalue(L, -1);  // make copy of module (call result)
    lhat_setfield(L, -3, modname);  // LOADED[modname] = module
  }
  lhat_remove(L, -2);  // remove LOADED table
  if (glb) {
    lhat_pushvalue(L, -1);  // copy of module
    lhat_setglobal(L, modname);  // _G[modname] = module
  }
}


LHATLIB_API const char *lhatL_gsub (lhat_State *L, const char *s, const char *p,
                                                               const char *r) {
  const char *wild;
  size_t l = strlen(p);
  lhatL_Buffer b;
  lhatL_buffinit(L, &b);
  while ((wild = strstr(s, p)) != NULL) {
    lhatL_addlstring(&b, s, wild - s);  // push prefix
    lhatL_addstring(&b, r);  // push replacement in place of pattern
    s = wild + l;  // continue after 'p'
  }
  lhatL_addstring(&b, s);  // push last suffix
  lhatL_pushresult(&b);
  return lhat_tostring(L, -1);
}


static void *l_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud; (void)osize;  // not used
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  else
    return realloc(ptr, nsize);
}


static int panic (lhat_State *L) {
  lhat_writestringerror("PANIC: unprotected error in call to Lhat API (%s)\n",
                        lhat_tostring(L, -1));
  return 0;  // return to Lhat to abort
}


LHATLIB_API lhat_State *lhatL_newstate (void) {
  lhat_State *L = lhat_newstate(l_alloc, NULL);
  if (L) lhat_atpanic(L, &panic);
  return L;
}


LHATLIB_API void lhatL_checkversion_ (lhat_State *L, lhat_Number ver, size_t sz) {
  const lhat_Number *v = lhat_version(L);
  if (sz != LHATL_NUMSIZES)  // check numeric types
    lhatL_error(L, "core and library have incompatible numeric types");
  if (v != lhat_version(NULL))
    lhatL_error(L, "multiple Lhat VMs detected");
  else if (*v != ver)
    lhatL_error(L, "version mismatch: app. needs %f, Lhat core provides %f",
                  (LHATI_UACNUMBER)ver, (LHATI_UACNUMBER)*v);
}

