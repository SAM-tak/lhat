/*
** $Id: lundump.h,v 1.45 2015/09/08 15:41:05 roberto Exp $
** load precompiled Lhat chunks
** See Copyright Notice in lhat.h
*/

#ifndef lundump_h
#define lundump_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/* data to catch conversion errors */
#define LHATC_DATA	"\x19\x93\r\n\x1a\n"

#define LHATC_INT	0x5678
#define LHATC_NUM	cast_num(370.5)

#define LHATC_VERSION	((LHAT_VERSION_MAJOR<<8)|LHAT_VERSION_MINOR)
#define LHATC_FORMAT	0	/* this is the official format */

/* load one chunk; from lundump.c */
LHATI_FUNC LClosure* lhatU_undump (lhat_State* L, ZIO* Z, const char* name);

/* dump one chunk; from ldump.c */
LHATI_FUNC int lhatU_dump (lhat_State* L, const Proto* f, lhat_Writer w,
                         void* data, int strip);

#endif
