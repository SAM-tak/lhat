// L^ (lhat) -- sockets, on whichever system.
//
// The core asks for none of this, and neither does the language: this is for
// what is built beside it. The debug adapter (dap/) listens for one debugger
// over TCP on the loopback, because DAP travels that way; std.net (11 章)
// sends and receives UDP datagrams. So it lives in port/ beside thread.h and
// is a target of its own that `lhat` does not link.

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

// Winsock needs a startup before any other call here and a cleanup after;
// elsewhere both do nothing. Winsock counts them, so each user pairs its own
// and the last cleanup is the one that takes effect.
bool lhat_socket_startup(void);
void lhat_socket_cleanup(void);

// ---------------------------------------------------------------------------
// TCP, loopback only. The adapter serves one debugger on the same machine;
// binding 127.0.0.1 is the whole of that intent.

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

// ---------------------------------------------------------------------------
// UDP

// An address of either family, held the way the system spells one.
typedef struct {
    uint64_t storage[16];  // room for a sockaddr_storage
    uint32_t length;
} LhatSocketAddress;

typedef enum {
    LHAT_SOCKET_DONE,
    LHAT_SOCKET_EMPTY,  // nothing has arrived: the receive would have blocked
    LHAT_SOCKET_IN_USE,
    LHAT_SOCKET_UNREACHABLE,
    LHAT_SOCKET_TOO_LARGE,
    LHAT_SOCKET_FAILED
} LhatSocketStatus;

// `host` by name or number, IPv4 or IPv6, the first answer the resolver
// gives. `passive` reads an empty host as every local address. Blocks while
// a name is looked up. False when nothing was found.
bool lhat_socket_resolve(const char *host, uint16_t port, bool passive,
                         LhatSocketAddress *out);

// A datagram socket of the family `address` is in, which never blocks:
// a receive with nothing waiting answers LHAT_SOCKET_EMPTY.
bool lhat_socket_udp(LhatSocket *out, const LhatSocketAddress *address);

LhatSocketStatus lhat_socket_bind(LhatSocket socket,
                                  const LhatSocketAddress *address);
LhatSocketStatus lhat_socket_send_to(LhatSocket socket, const void *bytes,
                                     size_t size, const LhatSocketAddress *to);

// One datagram into `buffer`, its size in `*got` and its sender in `*from`.
LhatSocketStatus lhat_socket_receive_from(LhatSocket socket, void *buffer,
                                          size_t size, size_t *got,
                                          LhatSocketAddress *from);

bool lhat_socket_set_broadcast(LhatSocket socket, bool on);

// Where the socket is bound -- the port the system picked for port 0.
bool lhat_socket_local(LhatSocket socket, LhatSocketAddress *out);

// The numeric host ("192.168.0.12", "::1") and the port.
bool lhat_socket_address_text(const LhatSocketAddress *address, char *host,
                              size_t capacity, uint16_t *port);

#ifdef __cplusplus
}
#endif

#endif  // LHAT_PORT_SOCKET_H
