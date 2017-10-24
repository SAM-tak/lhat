//
// load precompiled L^ chunks
// See Copyright Notice in lhat.h
//

#define LHAT_CORE

#include "lprefix.h"

#include <string.h>

#include "lhat.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "lundump.h"
#include "lzio.h"


#if !defined(lhati_verifycode)
# define lhati_verifycode(L,b,f)  // empty
#endif


typedef struct {
    lhat_State *L;
    ZIO *Z;
    const char *name;
} LoadState;


static l_noret error(LoadState *S, const char *why)
{
    lhatO_pushfstring(S->L, "%s: %s precompiled chunk", S->name, why);
    lhatD_throw(S->L, LHAT_ERRSYNTAX);
}


//
// All high-level loads go through LoadVector; you can change it to
// adapt to the endianness of the input
//
#define LoadVector(S,b,n)	LoadBlock(S,b,(n)*sizeof((b)[0]))

static void LoadBlock(LoadState *S, void *b, size_t size)
{
    if(lhatZ_read(S->Z, b, size) != 0)
        error(S, "truncated");
}


#define LoadVar(S,x)		LoadVector(S,&x,1)


static lu_byte LoadByte(LoadState *S)
{
    lu_byte x;
    LoadVar(S, x);
    return x;
}


static int LoadInt(LoadState *S)
{
    int x;
    LoadVar(S, x);
    return x;
}


static lhat_Number LoadNumber(LoadState *S)
{
    lhat_Number x;
    LoadVar(S, x);
    return x;
}


static lhat_Integer LoadInteger(LoadState *S)
{
    lhat_Integer x;
    LoadVar(S, x);
    return x;
}


static TString *LoadString(LoadState *S)
{
    size_t size = LoadByte(S);
    if(size == 0xFF)
        LoadVar(S, size);
    if(size == 0)
        return NULL;
    else if(--size <= LHATI_MAXSHORTLEN) {  // short string?
        char buff[LHATI_MAXSHORTLEN];
        LoadVector(S, buff, size);
        return lhatS_newlstr(S->L, buff, size);
    }
    else {  // long string
        TString *ts = lhatS_createlngstrobj(S->L, size);
        LoadVector(S, getstr(ts), size);  // load directly in final place
        return ts;
    }
}


static void LoadCode(LoadState *S, Proto *f)
{
    int n = LoadInt(S);
    f->code = lhatM_newvector(S->L, n, Instruction);
    f->sizecode = n;
    LoadVector(S, f->code, n);
}


static void LoadFunction(LoadState *S, Proto *f, TString *psource);


static void LoadConstants(LoadState *S, Proto *f)
{
    int n = LoadInt(S);
    f->k = lhatM_newvector(S->L, n, TValue);
    f->sizek = n;
    for(int i = 0; i < n; i++)
        setnilvalue(&f->k[i]);
    for(int i = 0; i < n; i++) {
        TValue *o = &f->k[i];
        int t = LoadByte(S);
        switch(t) {
        case LHAT_TNIL:
            setnilvalue(o);
            break;
        case LHAT_TBOOLEAN:
            setbvalue(o, LoadByte(S));
            break;
        case LHAT_TNUMFLT:
            setfltvalue(o, LoadNumber(S));
            break;
        case LHAT_TNUMINT:
            setivalue(o, LoadInteger(S));
            break;
        case LHAT_TSHRSTR:
        case LHAT_TLNGSTR:
            setsvalue2n(S->L, o, LoadString(S));
            break;
        default:
            lhat_assert(0);
        }
    }
}


static void LoadProtos(LoadState *S, Proto *f)
{
    int n = LoadInt(S);
    f->p = lhatM_newvector(S->L, n, Proto *);
    f->sizep = n;
    for(int i = 0; i < n; i++)
        f->p[i] = NULL;
    for(int i = 0; i < n; i++) {
        f->p[i] = lhatF_newproto(S->L);
        LoadFunction(S, f->p[i], f->source);
    }
}


static void LoadUpvalues(LoadState *S, Proto *f)
{
    int n = LoadInt(S);
    f->upvalues = lhatM_newvector(S->L, n, Upvaldesc);
    f->sizeupvalues = n;
    for(int i = 0; i < n; i++)
        f->upvalues[i].name = NULL;
    for(int i = 0; i < n; i++) {
        f->upvalues[i].instack = LoadByte(S);
        f->upvalues[i].idx = LoadByte(S);
    }
}


static void LoadDebug(LoadState *S, Proto *f)
{
    int n = LoadInt(S);
    f->lineinfo = lhatM_newvector(S->L, n, int);
    f->sizelineinfo = n;
    LoadVector(S, f->lineinfo, n);
    n = LoadInt(S);
    f->locvars = lhatM_newvector(S->L, n, LocVar);
    f->sizelocvars = n;
    for(int i = 0; i < n; i++)
        f->locvars[i].varname = NULL;
    for(int i = 0; i < n; i++) {
        f->locvars[i].varname = LoadString(S);
        f->locvars[i].startpc = LoadInt(S);
        f->locvars[i].endpc = LoadInt(S);
    }
    n = LoadInt(S);
    for(int i = 0; i < n; i++)
        f->upvalues[i].name = LoadString(S);
}


static void LoadFunction(LoadState *S, Proto *f, TString *psource)
{
    f->source = LoadString(S);
    if(f->source == NULL)  // no source in dump?
        f->source = psource;  // reuse parent's source
    f->linedefined = LoadInt(S);
    f->lastlinedefined = LoadInt(S);
    f->numparams = LoadByte(S);
    f->is_vararg = LoadByte(S);
    f->maxstacksize = LoadByte(S);
    LoadCode(S, f);
    LoadConstants(S, f);
    LoadUpvalues(S, f);
    LoadProtos(S, f);
    LoadDebug(S, f);
}


static void checkliteral(LoadState *S, const char *s, const char *msg)
{
    char buff[sizeof(LHAT_SIGNATURE) + sizeof(LHATC_DATA)]; // larger than both
    size_t len = strlen(s);
    LoadVector(S, buff, len);
    if(memcmp(s, buff, len) != 0)
        error(S, msg);
}


static void fchecksize(LoadState *S, size_t size, const char *tname)
{
    if(LoadByte(S) != size)
        error(S, lhatO_pushfstring(S->L, "%s size mismatch in", tname));
}


#define checksize(S,t)	fchecksize(S,sizeof(t),#t)

static void checkHeader(LoadState *S)
{
    checkliteral(S, LHAT_SIGNATURE + 1, "not a");  // 1st char already checked
    if(LoadByte(S) != LHATC_VERSION)
        error(S, "version mismatch in");
    if(LoadByte(S) != LHATC_FORMAT)
        error(S, "format mismatch in");
    checkliteral(S, LHATC_DATA, "corrupted");
    checksize(S, int);
    checksize(S, size_t);
    checksize(S, Instruction);
    checksize(S, lhat_Integer);
    checksize(S, lhat_Number);
    if(LoadInteger(S) != LHATC_INT)
        error(S, "endianness mismatch in");
    if(LoadNumber(S) != LHATC_NUM)
        error(S, "float format mismatch in");
}


//
// load precompiled chunk
//
LClosure *lhatU_undump(lhat_State *L, ZIO *Z, const char *name)
{
    LoadState S;
    LClosure *cl;
    if(*name == '@' || *name == '=')
        S.name = name + 1;
    else if(*name == LHAT_SIGNATURE[0])
        S.name = "binary string";
    else
        S.name = name;
    S.L = L;
    S.Z = Z;
    checkHeader(&S);
    cl = lhatF_newLclosure(L, LoadByte(&S));
    setclLvalue(L, L->top, cl);
    lhatD_inctop(L);
    cl->p = lhatF_newproto(L);
    LoadFunction(&S, cl->p, NULL);
    lhat_assert(cl->nupvalues == cl->p->sizeupvalues);
    lhati_verifycode(L, buff, cl->p);
    return cl;
}
