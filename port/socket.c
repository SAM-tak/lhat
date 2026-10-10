// L^ (lhat) -- sockets, on Windows (Winsock) and on the systems that share
// the BSD sockets API (everything else here).

#include "port/socket.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET NativeSocket;
#define LHAT_BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int NativeSocket;
#define LHAT_BAD_SOCKET (-1)
#endif

static NativeSocket native(LhatSocket socket)
{
    return (NativeSocket)socket.handle;
}

static LhatSocket wrap(NativeSocket socket)
{
    LhatSocket out;
    out.handle = (intptr_t)socket;
    return out;
}

bool lhat_socket_startup(void)
{
#ifdef _WIN32
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void lhat_socket_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

bool lhat_socket_listen(LhatSocket *out, uint16_t port)
{
    NativeSocket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == LHAT_BAD_SOCKET) {
        return false;
    }
    // So a debugger reconnecting to the same port right after a session does
    // not trip over the kernel's lingering bind.
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes,
               sizeof yes);

    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(listener, 1) != 0) {
        lhat_socket_close(wrap(listener));
        return false;
    }
    *out = wrap(listener);
    return true;
}

bool lhat_socket_accept(LhatSocket listener, LhatSocket *out)
{
    NativeSocket peer = accept(native(listener), NULL, NULL);
    if (peer == LHAT_BAD_SOCKET) {
        return false;
    }
    *out = wrap(peer);
    return true;
}

bool lhat_socket_readable(LhatSocket socket, int timeout_ms)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(native(socket), &readable);
    struct timeval tv;
    struct timeval *timeout = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        timeout = &tv;
    }
    // The first argument is ignored on Windows and is the highest fd plus one
    // elsewhere; a single-socket set makes that the socket plus one.
    int nfds = (int)native(socket) + 1;
    return select(nfds, &readable, NULL, NULL, timeout) > 0;
}

long lhat_socket_recv(LhatSocket socket, char *buffer, size_t size)
{
#ifdef _WIN32
    int got = recv(native(socket), buffer, (int)size, 0);
#else
    ssize_t got = recv(native(socket), buffer, size, 0);
#endif
    return (long)got;
}

bool lhat_socket_send_all(LhatSocket socket, const char *bytes, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        int n = send(native(socket), bytes + sent, (int)(size - sent), 0);
#else
        ssize_t n = send(native(socket), bytes + sent, size - sent, 0);
#endif
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

void lhat_socket_close(LhatSocket socket)
{
#ifdef _WIN32
    closesocket(native(socket));
#else
    close(native(socket));
#endif
}

// ---------------------------------------------------------------------------
// UDP

_Static_assert(sizeof(((LhatSocketAddress *)0)->storage) >= sizeof(struct sockaddr_storage),
               "LhatSocketAddress has to hold any address");

static const struct sockaddr *address_of(const LhatSocketAddress *address)
{
    return (const struct sockaddr *)address->storage;
}

// What the last call's failure means here.
static LhatSocketStatus last_status(void)
{
#ifdef _WIN32
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK:
        return LHAT_SOCKET_EMPTY;
    case WSAEADDRINUSE:
        return LHAT_SOCKET_IN_USE;
    case WSAEMSGSIZE:
        return LHAT_SOCKET_TOO_LARGE;
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAENETDOWN:
    case WSAECONNRESET:
    case WSAEADDRNOTAVAIL:
    case WSAEAFNOSUPPORT:
        return LHAT_SOCKET_UNREACHABLE;
    default:
        return LHAT_SOCKET_FAILED;
    }
#else
    switch (errno) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return LHAT_SOCKET_EMPTY;
    case EADDRINUSE:
        return LHAT_SOCKET_IN_USE;
    case EMSGSIZE:
        return LHAT_SOCKET_TOO_LARGE;
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
    case ECONNREFUSED:
    case EADDRNOTAVAIL:
    case EAFNOSUPPORT:
        return LHAT_SOCKET_UNREACHABLE;
    default:
        return LHAT_SOCKET_FAILED;
    }
#endif
}

bool lhat_socket_resolve(const char *host, uint16_t port, bool passive,
                         LhatSocketAddress *out)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    struct addrinfo *found = NULL;
    const char *name = passive && host[0] == '\0' ? NULL : host;
    if (getaddrinfo(name, "0", &hints, &found) != 0 || found == NULL) {
        return false;
    }
    bool fits = found->ai_addrlen <= sizeof out->storage;
    if (fits) {
        memset(out, 0, sizeof *out);
        memcpy(out->storage, found->ai_addr, found->ai_addrlen);
        out->length = (uint32_t)found->ai_addrlen;
        struct sockaddr *address = (struct sockaddr *)out->storage;
        if (address->sa_family == AF_INET6) {
            ((struct sockaddr_in6 *)address)->sin6_port = htons(port);
        } else {
            ((struct sockaddr_in *)address)->sin_port = htons(port);
        }
    }
    freeaddrinfo(found);
    return fits;
}

bool lhat_socket_udp(LhatSocket *out, const LhatSocketAddress *address)
{
    NativeSocket s = socket(address_of(address)->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == LHAT_BAD_SOCKET) {
        return false;
    }
#ifdef _WIN32
    u_long yes = 1;
    bool ok = ioctlsocket(s, FIONBIO, &yes) == 0;
    // A datagram that met a closed port answers with ICMP, and Windows hands
    // that to the next receive as a reset -- which a socket with no
    // connection has nothing to do with. Turned off, as every UDP game does.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, NULL, 0, &returned, NULL, NULL);
#else
    int flags = fcntl(s, F_GETFL, 0);
    bool ok = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    if (!ok) {
        lhat_socket_close(wrap(s));
        return false;
    }
    *out = wrap(s);
    return true;
}

LhatSocketStatus lhat_socket_bind(LhatSocket socket, const LhatSocketAddress *address)
{
    return bind(native(socket), address_of(address), (int)address->length) == 0
               ? LHAT_SOCKET_DONE
               : last_status();
}

LhatSocketStatus lhat_socket_send_to(LhatSocket socket, const void *bytes, size_t size,
                                     const LhatSocketAddress *to)
{
#ifdef _WIN32
    if (size > INT_MAX) {
        return LHAT_SOCKET_TOO_LARGE;
    }
    int sent = sendto(native(socket), (const char *)bytes, (int)size, 0, address_of(to),
                      (int)to->length);
#else
    ssize_t sent = sendto(native(socket), bytes, size, 0, address_of(to), to->length);
#endif
    if (sent < 0) {
        LhatSocketStatus status = last_status();
        // A full send buffer drops the datagram, which UDP is allowed to do.
        return status == LHAT_SOCKET_EMPTY ? LHAT_SOCKET_DONE : status;
    }
    return LHAT_SOCKET_DONE;
}

LhatSocketStatus lhat_socket_receive_from(LhatSocket socket, void *buffer, size_t size,
                                          size_t *got, LhatSocketAddress *from)
{
    memset(from, 0, sizeof *from);
#ifdef _WIN32
    int length = (int)sizeof from->storage;
    int received = recvfrom(native(socket), (char *)buffer, size > INT_MAX ? INT_MAX : (int)size,
                            0, (struct sockaddr *)from->storage, &length);
#else
    socklen_t length = (socklen_t)sizeof from->storage;
    ssize_t received = recvfrom(native(socket), buffer, size, 0,
                                (struct sockaddr *)from->storage, &length);
#endif
    if (received < 0) {
        return last_status();
    }
    from->length = (uint32_t)length;
    *got = (size_t)received;
    return LHAT_SOCKET_DONE;
}

bool lhat_socket_set_broadcast(LhatSocket socket, bool on)
{
    int value = on ? 1 : 0;
    return setsockopt(native(socket), SOL_SOCKET, SO_BROADCAST, (const char *)&value,
                      sizeof value) == 0;
}

bool lhat_socket_local(LhatSocket socket, LhatSocketAddress *out)
{
    memset(out, 0, sizeof *out);
#ifdef _WIN32
    int length = (int)sizeof out->storage;
#else
    socklen_t length = (socklen_t)sizeof out->storage;
#endif
    if (getsockname(native(socket), (struct sockaddr *)out->storage, &length) != 0) {
        return false;
    }
    out->length = (uint32_t)length;
    return true;
}

bool lhat_socket_address_text(const LhatSocketAddress *address, char *host,
                              size_t capacity, uint16_t *port)
{
    char service[16];
    if (getnameinfo(address_of(address), (socklen_t)address->length, host, (socklen_t)capacity,
                    service, sizeof service, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return false;
    }
    *port = (uint16_t)strtoul(service, NULL, 10);
    return true;
}
