// L^ (lhat) -- a listening TCP socket on the loopback, on whichever system.
//
// The core asks for none of this, and neither does the language: this is for
// what is built beside it -- the debug adapter (dap/), which a debugger
// reaches over a socket because DAP travels that way. So it lives in port/
// beside thread.h and is a target of its own that `lhat` does not link.
//
// Loopback only. The adapter serves one debugger on the same machine (a
// VS Code extension it was spawned by, or an editor); nothing here is meant
// to face a network, and binding 127.0.0.1 is the whole of that intent.

#ifndef LHAT_PORT_SOCKET_H
#define LHAT_PORT_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The system's socket handle, wrapped so a caller passes it by value without
// naming SOCKET or int. An invalid one is what a failed accept leaves.
typedef struct {
    intptr_t handle;
} LhatSocket;

// Winsock needs a process-wide startup; elsewhere this is a no-op that
// answers true. Call once before any other call here, and cleanup once at
// the end.
bool lhat_socket_startup(void);
void lhat_socket_cleanup(void);

// Listens on 127.0.0.1:`port`. false when the port could not be taken.
bool lhat_socket_listen(LhatSocket *out, uint16_t port);

// Blocks for one connection and answers it. false when the listener failed.
bool lhat_socket_accept(LhatSocket listener, LhatSocket *out);

// Whether a recv would return without blocking -- data waiting, or the peer
// gone. `timeout_ms` of 0 polls and returns at once; a negative one waits
// without limit. The adapter polls this between lines so a pause can arrive
// mid-run without a thread to receive it.
bool lhat_socket_readable(LhatSocket socket, int timeout_ms);

// Up to `size` bytes into `buffer`. 0 when the peer closed cleanly, -1 on
// error -- the framing (transport.c) treats either as end of input.
long lhat_socket_recv(LhatSocket socket, char *buffer, size_t size);

// All `size` bytes, retrying a short send. false when the peer is gone.
bool lhat_socket_send_all(LhatSocket socket, const char *bytes, size_t size);

void lhat_socket_close(LhatSocket socket);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_PORT_SOCKET_H
