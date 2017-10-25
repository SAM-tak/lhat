#ifndef lzbuf_h
#define lzbuf_h
//
// Buffered streams
// See Copyright Notice in lhat.h
//

#include "lhat.h"

#include "lmem.h"

enum {
	EOZ	 = -1			// end of stream
};

typedef struct ZBuf {
	size_t n;			// bytes still unread
	const char *p;		// current position in buffer
	lhat_Reader reader;	// reader function
	void *data;			// additional data
	lhat_State *L;		// L^ state (for reader)
} ZBuf;

LHATI_FUNC void lhatZ_init(lhat_State *L, ZBuf *z, lhat_Reader reader, void *data);
LHATI_FUNC size_t lhatZ_read(ZBuf* z, void *b, size_t n);	// read next n bytes

LHATI_FUNC int lhatZ_fill(ZBuf *z);

inline int zgetc(ZBuf *z)
{
	return (z->n--) > 0 ? cast_uchar(*z->p++) : lhatZ_fill(z);
}

#endif // !lzbuf_h