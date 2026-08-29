// L^ (lhat) -- a loopback TCP socket, on Windows (Winsock) and on the
// systems that share the BSD sockets API (everything else here).

#include "port/socket.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET NativeSocket;
#define LHAT_BAD_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
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
