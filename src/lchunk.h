#ifndef lchunk_h
#define lchunk_h
//
// load precompiled L^ chunks
// See Copyright Notice in lhat.h
//

#include "llimits.h"
#include "lobject.h"
#include "lzbuf.h"

// data to catch conversion errors
#define LHATC_DATA	"\x19\x93\r\n\x1a\n"

#define LHATC_INT	0x5678
#define LHATC_NUM	cast_num(370.5)

#define LHATC_VERSION	((LHAT_VERSION_MAJOR<<8)|LHAT_VERSION_MINOR)
#define LHATC_FORMAT	0	// this is the official format

// load one chunk; from lundump.c
LHATI_FUNC LClosure *lhatU_undump(lhat_State *L, ZBuf* Z, const char *name);

// dump one chunk; from ldump.c
LHATI_FUNC int lhatU_dump(lhat_State *L, const Proto *f, lhat_Writer w, void *data, int strip);

#endif // !lchunk_h
