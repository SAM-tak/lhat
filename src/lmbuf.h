#ifndef lmbuf_h
#define lmbuf_h
//
// Generic Buffer
// See Copyright Notice in lhat.h
//

#include "lhat.h"

#include "lmem.h"

typedef struct MBuffer {
	char *buffer;
	size_t n;
	size_t buffsize;
} MBuffer;

inline void lhatM_initbuffer(MBuffer *buff)
{
	buff->buffer = NULL;
	buff->buffsize = 0;
}

inline char *lhatM_buffer(MBuffer *buff)
{
	return buff->buffer;
}

inline size_t lhatM_sizebuffer(MBuffer *buff)
{
	return buff->buffsize;
}

inline size_t lhatM_bufflen(MBuffer *buff)
{
	return buff->n;
}

inline void lhatM_buffremove(MBuffer *buff, size_t i)
{
	buff->n -= i;
}

inline void lhatM_resetbuffer(MBuffer *buff)
{
	buff->n = 0;
}

inline void lhatM_resizebuffer(MBuffer *buff, size_t size, lhat_State *L)
{
	buff->buffer = lhatM_reallocvchar(L, buff->buffer, buff->buffsize, size);
	buff->buffsize = size;
}

inline void lhatM_freebuffer(MBuffer *buff, lhat_State *L)
{
	lhatM_resizebuffer(buff, 0, L);
}

#endif // !lmbuf_h