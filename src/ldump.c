/*
** $Id: ldump.c,v 2.37 2015/10/08 15:53:49 roberto Exp $
** save precompiled Lhat chunks
** See Copyright Notice in lhat.h
*/

#define ldump_c
#define LHAT_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lhat.h"

#include "lobject.h"
#include "lstate.h"
#include "lundump.h"


typedef struct {
  lhat_State *L;
  lhat_Writer writer;
  void *data;
  int strip;
  int status;
} DumpState;


/*
** All high-level dumps go through DumpVector; you can change it to
** change the endianness of the result
*/
#define DumpVector(v,n,D)	DumpBlock(v,(n)*sizeof((v)[0]),D)

#define DumpLiteral(s,D)	DumpBlock(s, sizeof(s) - sizeof(char), D)


static void DumpBlock (const void *b, size_t size, DumpState *D) {
  if (D->status == 0 && size > 0) {
    lhat_unlock(D->L);
    D->status = (*D->writer)(D->L, b, size, D->data);
    lhat_lock(D->L);
  }
}


#define DumpVar(x,D)		DumpVector(&x,1,D)


static void DumpByte (int y, DumpState *D) {
  lu_byte x = (lu_byte)y;
  DumpVar(x, D);
}


static void DumpInt (int x, DumpState *D) {
  DumpVar(x, D);
}


static void DumpNumber (lhat_Number x, DumpState *D) {
  DumpVar(x, D);
}


static void DumpInteger (lhat_Integer x, DumpState *D) {
  DumpVar(x, D);
}


static void DumpString (const TString *s, DumpState *D) {
  if (s == NULL)
    DumpByte(0, D);
  else {
    size_t size = tsslen(s) + 1;  /* include trailing '\0' */
    const char *str = getstr(s);
    if (size < 0xFF)
      DumpByte(cast_int(size), D);
    else {
      DumpByte(0xFF, D);
      DumpVar(size, D);
    }
    DumpVector(str, size - 1, D);  /* no need to save '\0' */
  }
}


static void DumpCode (const Proto *f, DumpState *D) {
  DumpInt(f->sizecode, D);
  DumpVector(f->code, f->sizecode, D);
}


static void DumpFunction(const Proto *f, TString *psource, DumpState *D);

static void DumpConstants (const Proto *f, DumpState *D) {
  int i;
  int n = f->sizek;
  DumpInt(n, D);
  for (i = 0; i < n; i++) {
    const TValue *o = &f->k[i];
    DumpByte(ttype(o), D);
    switch (ttype(o)) {
    case LHAT_TNIL:
      break;
    case LHAT_TBOOLEAN:
      DumpByte(bvalue(o), D);
      break;
    case LHAT_TNUMFLT:
      DumpNumber(fltvalue(o), D);
      break;
    case LHAT_TNUMINT:
      DumpInteger(ivalue(o), D);
      break;
    case LHAT_TSHRSTR:
    case LHAT_TLNGSTR:
      DumpString(tsvalue(o), D);
      break;
    default:
      lhat_assert(0);
    }
  }
}


static void DumpProtos (const Proto *f, DumpState *D) {
  int i;
  int n = f->sizep;
  DumpInt(n, D);
  for (i = 0; i < n; i++)
    DumpFunction(f->p[i], f->source, D);
}


static void DumpUpvalues (const Proto *f, DumpState *D) {
  int i, n = f->sizeupvalues;
  DumpInt(n, D);
  for (i = 0; i < n; i++) {
    DumpByte(f->upvalues[i].instack, D);
    DumpByte(f->upvalues[i].idx, D);
  }
}


static void DumpDebug (const Proto *f, DumpState *D) {
  int i, n;
  n = (D->strip) ? 0 : f->sizelineinfo;
  DumpInt(n, D);
  DumpVector(f->lineinfo, n, D);
  n = (D->strip) ? 0 : f->sizelocvars;
  DumpInt(n, D);
  for (i = 0; i < n; i++) {
    DumpString(f->locvars[i].varname, D);
    DumpInt(f->locvars[i].startpc, D);
    DumpInt(f->locvars[i].endpc, D);
  }
  n = (D->strip) ? 0 : f->sizeupvalues;
  DumpInt(n, D);
  for (i = 0; i < n; i++)
    DumpString(f->upvalues[i].name, D);
}


static void DumpFunction (const Proto *f, TString *psource, DumpState *D) {
  if (D->strip || f->source == psource)
    DumpString(NULL, D);  /* no debug info or same source as its parent */
  else
    DumpString(f->source, D);
  DumpInt(f->linedefined, D);
  DumpInt(f->lastlinedefined, D);
  DumpByte(f->numparams, D);
  DumpByte(f->is_vararg, D);
  DumpByte(f->maxstacksize, D);
  DumpCode(f, D);
  DumpConstants(f, D);
  DumpUpvalues(f, D);
  DumpProtos(f, D);
  DumpDebug(f, D);
}


static void DumpHeader (DumpState *D) {
  DumpLiteral(LHAT_SIGNATURE, D);
  DumpByte(LHATC_VERSION, D);
  DumpByte(LHATC_FORMAT, D);
  DumpLiteral(LHATC_DATA, D);
  DumpByte(sizeof(int), D);
  DumpByte(sizeof(size_t), D);
  DumpByte(sizeof(Instruction), D);
  DumpByte(sizeof(lhat_Integer), D);
  DumpByte(sizeof(lhat_Number), D);
  DumpInteger(LHATC_INT, D);
  DumpNumber(LHATC_NUM, D);
}


/*
** dump Lhat function as precompiled chunk
*/
int lhatU_dump(lhat_State *L, const Proto *f, lhat_Writer w, void *data,
              int strip) {
  DumpState D;
  D.L = L;
  D.writer = w;
  D.data = data;
  D.strip = strip;
  D.status = 0;
  DumpHeader(&D);
  DumpByte(f->sizeupvalues, &D);
  DumpFunction(f, NULL, &D);
  return D.status;
}

