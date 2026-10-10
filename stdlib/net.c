// L^ (lhat) -- sample standard library: std.net (11 章).
//
// A std.net.Udp holds one socket, made when it is first given an address --
// bind, setPeer's first send, or sendTo -- in that address's family. Every
// socket is non-blocking: receive answers nil^ in the data's place when
// nothing has arrived (11 の 4.2), and a loop reads until it does.
//
// A failure of the network came from outside and answers std.net.Error; a
// port outside 0..65535, or a send with no peer named, is the writer's
// mistake and panics.

#include "net.h"

#include "binary.h"
#include "port/socket.h"

#include <stdio.h>
#include <string.h>

// N2: one receive takes the largest datagram UDP carries, so nothing that
// arrives is cut short.
#define RECEIVE_ROOM 65536

typedef struct {
    const LhatHostDataTag *tag;
    const LhatErrorKind *in_use;
    const LhatErrorKind *resolve;
    const LhatErrorKind *unreachable;
    const LhatErrorKind *too_large;
    const LhatErrorKind *closed;
    const LhatErrorKind *failed;
} NetModule;

static NetModule shared;

typedef struct {
    LhatSocket socket;
    bool open;
    bool broadcast;
    bool has_peer;
    LhatSocketAddress peer;
    uint8_t *room;  // RECEIVE_ROOM bytes, made by the first receive
} Udp;

static Udp *live(LhatValue value)
{
    Udp *udp = (Udp *)lhat_hostdata_pointer(value, shared.tag);
    return udp != NULL && !((const LhatHostData *)lhat_as_object(value))->released ? udp : NULL;
}

static bool fail(LhatMachine *machine, const LhatErrorKind *kind, const char *message,
                 LhatValue *answer)
{
    if (!lhat_machine_make_error(machine, kind, message, lhat_nil(), answer)) {
        lhat_machine_panic_text(machine, "out of memory");
        return false;
    }
    return true;
}

static const LhatErrorKind *error_of(LhatSocketStatus status, const char **message)
{
    switch (status) {
    case LHAT_SOCKET_IN_USE:
        *message = "the address is in use";
        return shared.in_use;
    case LHAT_SOCKET_UNREACHABLE:
        *message = "the address cannot be reached";
        return shared.unreachable;
    case LHAT_SOCKET_TOO_LARGE:
        *message = "the datagram is too large";
        return shared.too_large;
    default:
        *message = "the socket failed";
        return shared.failed;
    }
}

// Answers nil^ for DONE, and the error otherwise.
static void answer_status(LhatMachine *machine, LhatSocketStatus status, LhatValue *answers,
                          int *answer_count)
{
    if (status == LHAT_SOCKET_DONE) {
        return;
    }
    const char *message = NULL;
    const LhatErrorKind *kind = error_of(status, &message);
    if (fail(machine, kind, message, &answers[0])) {
        *answer_count = 1;
    }
}

// A host name as a C string. A string with a NUL in it names nothing, which
// the resolver is left to say.
static bool host_argument(LhatValue value, char *out, size_t capacity)
{
    const LhatString *text = lhat_is_object_kind(value, LHAT_OBJECT_STRING)
                                 ? (const LhatString *)lhat_as_object(value)
                                 : NULL;
    if (text == NULL || text->length >= capacity || memchr(text->text, '\0', text->length)) {
        return false;
    }
    memcpy(out, text->text, text->length);
    out[text->length] = '\0';
    return true;
}

static bool port_argument(LhatMachine *machine, LhatValue value, uint16_t *out)
{
    if (!lhat_is_integer(value) || lhat_as_integer(value) < 0 || lhat_as_integer(value) > 65535) {
        lhat_machine_panic_text(machine, "std.net: a port is an integer from 0 to 65535");
        return false;
    }
    *out = (uint16_t)lhat_as_integer(value);
    return true;
}

// The address `host` and `port` name, or false with the error in answers[0].
static bool resolve(LhatMachine *machine, LhatValue host, LhatValue port, bool passive,
                    LhatSocketAddress *out, LhatValue *answers, int *answer_count)
{
    char name[256];
    uint16_t number = 0;
    if (!port_argument(machine, port, &number)) {
        return false;
    }
    if (!host_argument(host, name, sizeof name) ||
        !lhat_socket_resolve(name, number, passive, out)) {
        if (fail(machine, shared.resolve, "the name could not be resolved", &answers[0])) {
            *answer_count = 1;
        }
        return false;
    }
    return true;
}

// The socket, made in `address`'s family when there is none yet.
static LhatSocketStatus opened(Udp *udp, const LhatSocketAddress *address)
{
    if (udp->open) {
        return LHAT_SOCKET_DONE;
    }
    if (!lhat_socket_udp(&udp->socket, address)) {
        return LHAT_SOCKET_FAILED;
    }
    udp->open = true;
    if (udp->broadcast && !lhat_socket_set_broadcast(udp->socket, true)) {
        return LHAT_SOCKET_FAILED;
    }
    return LHAT_SOCKET_DONE;
}

// What `data` holds, a string^ or a std.binary.Bytes.
static bool payload(LhatValue data, const void **bytes, size_t *size)
{
    if (lhat_is_object_kind(data, LHAT_OBJECT_STRING)) {
        const LhatString *text = (const LhatString *)lhat_as_object(data);
        *bytes = text->text;
        *size = text->length;
        return true;
    }
    LhatBinaryBytes *held = lhatstdlib_binary_bytes(data);
    if (held != NULL) {
        *bytes = held->data;
        *size = held->length;
        return true;
    }
    return false;
}

// Answers with the Closed error when `udp` was given back already.
static Udp *open_udp(LhatMachine *machine, LhatValue self, LhatValue *answers,
                     int *answer_count)
{
    Udp *udp = live(self);
    if (udp == NULL && fail(machine, shared.closed, "the socket is closed", &answers[0])) {
        *answer_count = 1;
    }
    return udp;
}

static void net_udp(LhatMachine *machine, void *context, const LhatValue *arguments,
                    size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)arguments;
    (void)count;
    if (!lhat_socket_startup()) {
        if (fail(machine, shared.failed, "the network could not be started", &answers[0])) {
            *answer_count = 1;
        }
        return;
    }
    Udp *udp = (Udp *)lhat_calloc(1, sizeof *udp);
    LhatValue out = lhat_nil();
    if (udp == NULL || !lhat_machine_make_hostdata(machine, shared.tag, udp, &out)) {
        lhat_free(udp);
        lhat_socket_cleanup();
        lhat_machine_panic_text(machine, "out of memory");
        return;
    }
    answers[0] = out;
    *answer_count = 1;
}

static void udp_bind(LhatMachine *machine, void *context, const LhatValue *arguments,
                     size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    Udp *udp = open_udp(machine, arguments[0], answers, answer_count);
    LhatSocketAddress address;
    if (udp == NULL ||
        !resolve(machine, arguments[1], arguments[2], true, &address, answers, answer_count)) {
        return;
    }
    LhatSocketStatus status = opened(udp, &address);
    if (status == LHAT_SOCKET_DONE) {
        status = lhat_socket_bind(udp->socket, &address);
    }
    answer_status(machine, status, answers, answer_count);
}

static void udp_set_peer(LhatMachine *machine, void *context, const LhatValue *arguments,
                         size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    Udp *udp = open_udp(machine, arguments[0], answers, answer_count);
    LhatSocketAddress address;
    if (udp != NULL &&
        resolve(machine, arguments[1], arguments[2], false, &address, answers, answer_count)) {
        udp->peer = address;
        udp->has_peer = true;
    }
}

static void send_to(LhatMachine *machine, Udp *udp, LhatValue data,
                    const LhatSocketAddress *to, LhatValue *answers, int *answer_count)
{
    const void *bytes = NULL;
    size_t size = 0;
    if (!payload(data, &bytes, &size)) {
        lhat_machine_panic_text(machine, "std.net: the bytes were disposed");
        return;
    }
    LhatSocketStatus status = opened(udp, to);
    if (status == LHAT_SOCKET_DONE) {
        status = lhat_socket_send_to(udp->socket, bytes, size, to);
    }
    answer_status(machine, status, answers, answer_count);
}

static void udp_send(LhatMachine *machine, void *context, const LhatValue *arguments,
                     size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    Udp *udp = open_udp(machine, arguments[0], answers, answer_count);
    if (udp == NULL) {
        return;
    }
    if (!udp->has_peer) {
        lhat_machine_panic_text(machine, "std.net: send needs a peer named by setPeer");
        return;
    }
    send_to(machine, udp, arguments[1], &udp->peer, answers, answer_count);
}

static void udp_send_to(LhatMachine *machine, void *context, const LhatValue *arguments,
                        size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    Udp *udp = open_udp(machine, arguments[0], answers, answer_count);
    LhatSocketAddress address;
    if (udp != NULL &&
        resolve(machine, arguments[2], arguments[3], false, &address, answers, answer_count)) {
        send_to(machine, udp, arguments[1], &address, answers, answer_count);
    }
}

// The "" every empty answer carries, made once per machine and kept on its
// host root: a game polls every frame, and each poll would otherwise leave a
// string behind.
static bool empty_text(LhatMachine *machine, LhatValue *out)
{
    static const char key[] = "std.net.empty";
    LhatTable *root = lhat_machine_host_root(machine);
    *out = lhat_table_get_bytes(root, key, sizeof key - 1);
    if (lhat_is_object_kind(*out, LHAT_OBJECT_STRING)) {
        return true;
    }
    LhatValue name = lhat_nil();
    bool refused = false;
    return lhat_machine_make_string(machine, key, sizeof key - 1, &name) &&
           lhat_machine_make_string(machine, "", 0, out) &&
           lhat_machine_table_set(machine, root, name, *out, &refused) && !refused;
}

// The host and port `address` names, or "" and 0 for none. Writes
// answers[0] and [1]; false once out of memory has panicked.
static bool answer_address(LhatMachine *machine, const LhatSocketAddress *address,
                           LhatValue *answers)
{
    char host[64];
    uint16_t port = 0;
    bool named = address != NULL && lhat_socket_address_text(address, host, sizeof host, &port);
    if (named ? !lhat_machine_make_string(machine, host, strlen(host), &answers[0])
              : !empty_text(machine, &answers[0])) {
        lhat_machine_panic_text(machine, "out of memory");
        return false;
    }
    answers[1] = lhat_integer(port);
    return true;
}

// 11 の 4.2: three answers -- the data or nil^, the sender's host and its
// port, which are "" and 0 when nothing arrived. An error is answered alone
// (02 の 13.8改2 pads it with nil^).
static void answer_three(LhatMachine *machine, LhatValue first,
                         const LhatSocketAddress *from, LhatValue *answers, int *answer_count)
{
    answers[0] = first;
    if (answer_address(machine, from, &answers[1])) {
        *answer_count = 3;
    }
}

// One datagram into `room`, or the answer saying why there is none. True
// when one arrived, with its size and sender.
static bool receive_into(LhatMachine *machine, Udp *udp, uint8_t *room, size_t *got,
                         LhatSocketAddress *from, LhatValue *answers, int *answer_count)
{
    LhatSocketStatus status = lhat_socket_receive_from(udp->socket, room, RECEIVE_ROOM, got, from);
    if (status == LHAT_SOCKET_DONE) {
        return true;
    }
    if (status == LHAT_SOCKET_EMPTY) {
        answer_three(machine, lhat_nil(), NULL, answers, answer_count);
    } else {
        answer_status(machine, status, answers, answer_count);
    }
    return false;
}

static void udp_receive(LhatMachine *machine, void *context, const LhatValue *arguments,
                        size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    Udp *udp = open_udp(machine, arguments[0], answers, answer_count);
    LhatValue first = lhat_nil();
    if (udp == NULL || !udp->open) {
        if (udp != NULL) {
            answer_three(machine, lhat_nil(), NULL, answers, answer_count);
        }
        return;
    }
    if (udp->room == NULL) {
        udp->room = (uint8_t *)lhat_alloc(RECEIVE_ROOM);
        if (udp->room == NULL) {
            lhat_machine_panic_text(machine, "out of memory");
            return;
        }
    }
    size_t got = 0;
    LhatSocketAddress from;
    if (!receive_into(machine, udp, udp->room, &got, &from, answers, answer_count)) {
        return;
    }
    if (!lhat_machine_make_string(machine, (const char *)udp->room, got, &first)) {
        lhat_machine_panic_text(machine, "out of memory");
        return;
    }
    answer_three(machine, first, &from, answers, answer_count);
}

static void udp_receive_into(LhatMachine *machine, void *context, const LhatValue *arguments,
                             size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    LhatBinaryBytes *bytes = lhatstdlib_binary_bytes(arguments[1]);
    if (bytes == NULL) {
        lhat_machine_panic_text(machine, "std.net: the bytes were disposed");
        return;
    }
    Udp *udp = open_udp(machine, arguments[0], answers, answer_count);
    if (udp == NULL || !udp->open) {
        if (udp != NULL) {
            answer_three(machine, lhat_nil(), NULL, answers, answer_count);
        }
        return;
    }
    if (!lhatstdlib_binary_bytes_resize(bytes, RECEIVE_ROOM)) {
        lhat_machine_panic_text(machine, "out of memory");
        return;
    }
    size_t got = 0;
    LhatSocketAddress from;
    bool arrived = receive_into(machine, udp, bytes->data, &got, &from, answers, answer_count);
    bytes->length = arrived ? got : 0;
    if (arrived) {
        answer_three(machine, arguments[1], &from, answers, answer_count);
    }
}

static void udp_get_local(LhatMachine *machine, void *context, const LhatValue *arguments,
                          size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    Udp *udp = live(arguments[0]);
    LhatSocketAddress local;
    bool bound = udp != NULL && udp->open && lhat_socket_local(udp->socket, &local);
    if (answer_address(machine, bound ? &local : NULL, answers)) {
        *answer_count = 2;
    }
}

static void udp_set_broadcast(LhatMachine *machine, void *context, const LhatValue *arguments,
                              size_t count, LhatValue *answers, int *answer_count)
{
    (void)context;
    (void)count;
    Udp *udp = open_udp(machine, arguments[0], answers, answer_count);
    if (udp == NULL) {
        return;
    }
    udp->broadcast = lhat_as_bool(arguments[1]);
    if (udp->open && !lhat_socket_set_broadcast(udp->socket, udp->broadcast)) {
        answer_status(machine, LHAT_SOCKET_FAILED, answers, answer_count);
    }
}

static void udp_dispose(LhatMachine *machine, void *context, const LhatValue *arguments,
                        size_t count, LhatValue *answers, int *answer_count)
{
    (void)machine;
    (void)context;
    (void)count;
    (void)answers;
    (void)answer_count;
    Udp *udp = (Udp *)lhat_hostdata_pointer(arguments[0], shared.tag);
    if (udp == NULL) {
        return;
    }
    if (udp->open) {
        lhat_socket_close(udp->socket);
    }
    lhat_socket_cleanup();
    lhat_free(udp->room);
    lhat_free(udp);
}

#define M "std.net"
#define DATA "string^|std.binary.Bytes"
// 02 の 13.8改2: an error stands for the whole answer, never one position.
#define FROM ", string^, number^)|std.net.Error;"

bool lhatstdlib_net_register(LhatProgram *program)
{
    if (lhat_lookup_host_context(program, M, NULL, "udp") != NULL) {
        return true;
    }
    if (!lhatstdlib_binary_register(program)) {
        return false;
    }
    static const char *const variants[] = {"AddressInUse", "Resolve",  "Unreachable",
                                           "TooLarge",     "Closed",   "Failed"};
    const LhatErrorKind *found[6] = {NULL};
    if (!lhat_register_error_kind(program, M, "Error", variants, 6, NULL, found)) {
        return false;
    }
    shared.in_use = found[0];
    shared.resolve = found[1];
    shared.unreachable = found[2];
    shared.too_large = found[3];
    shared.closed = found[4];
    shared.failed = found[5];
    shared.tag = lhat_register_hostdata_type(program, M, "Udp");
    if (shared.tag == NULL) {
        return false;
    }
    NetModule *module = &shared;
    return lhat_register_func(program, M, "udp", "p^ -> std.net.Udp|std.net.Error;", net_udp,
                              module) &&
           lhat_register_member(program, M, "Udp", "bind",
                                "p^self^, string^, number^ -> nil^|std.net.Error;", udp_bind,
                                module) &&
           lhat_register_member(program, M, "Udp", "setPeer",
                                "p^self^, string^, number^ -> nil^|std.net.Error;",
                                udp_set_peer, module) &&
           lhat_register_member(program, M, "Udp", "send",
                                "p^self^, " DATA " -> nil^|std.net.Error;", udp_send, module) &&
           lhat_register_member(program, M, "Udp", "sendTo",
                                "p^self^, " DATA ", string^, number^ -> nil^|std.net.Error;",
                                udp_send_to, module) &&
           lhat_register_member(program, M, "Udp", "receive",
                                "p^self^ -> (string^|nil^" FROM, udp_receive, module) &&
           lhat_register_member(program, M, "Udp", "receiveInto",
                                "p^self^, std.binary.Bytes -> (std.binary.Bytes|nil^" FROM,
                                udp_receive_into, module) &&
           lhat_register_member(program, M, "Udp", "getLocal", "p^self^ -> string^, number^;",
                                udp_get_local, module) &&
           lhat_register_member(program, M, "Udp", "setBroadcast",
                                "p^self^, bool^ -> nil^|std.net.Error;", udp_set_broadcast,
                                module) &&
           lhat_register_member(program, M, "Udp", "dispose", "p^self^;", udp_dispose, module);
}
