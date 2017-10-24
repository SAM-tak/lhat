#ifndef lzbuf_h
#define lzbuf_h
//
// Buffered streams
// See Copyright Notice in lhat.h
//

#include "lhat.h"

#include "lmem.h"


#define EOZ	(-1)			// end of stream

typedef struct ZBuf ZBuf;

#define zgetc(z)  (((z)->n--)>0 ?  cast_uchar(*(z)->p++) : lhatZ_fill(z))


typedef struct Mbuffer {
	char *buffer;
	size_t n;
	size_t buffsize;
} Mbuffer;

#define lhatZ_initbuffer(L, buff) ((buff)->buffer = NULL, (buff)->buffsize = 0)

#define lhatZ_buffer(buff)	((buff)->buffer)
#define lhatZ_sizebuffer(buff)	((buff)->buffsize)
#define lhatZ_bufflen(buff)	((buff)->n)

#define lhatZ_buffremove(buff,i)	((buff)->n -= (i))
#define lhatZ_resetbuffer(buff) ((buff)->n = 0)


#define lhatZ_resizebuffer(L, buff, size) \
	((buff)->buffer = lhatM_reallocvchar(L, (buff)->buffer, \
				(buff)->buffsize, size), \
	(buff)->buffsize = size)

#define lhatZ_freebuffer(L, buff)	lhatZ_resizebuffer(L, buff, 0)


LHATI_FUNC void lhatZ_init(lhat_State *L, ZBuf *z, lhat_Reader reader, void *data);
LHATI_FUNC size_t lhatZ_read(ZBuf* z, void *b, size_t n);	// read next n bytes

// --------- Private Part ------------------
struct ZBuf {
	size_t n;			// bytes still unread
	const char *p;		// current position in buffer
	lhat_Reader reader;		// reader function
	void *data;			// additional data
	lhat_State *L;			// L^ state (for reader)
};

LHATI_FUNC int lhatZ_fill(ZBuf *z);

#endif // !lzbuf_h