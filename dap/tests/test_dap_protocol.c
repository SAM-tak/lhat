// L^ (lhat) -- the DAP envelope and the framing under it, over a memory
// stream so no socket is needed. What is pinned is that a request parses,
// a response carries the request's seq and command back, and an event is
// framed the way a reader on the other end would expect.

#include <stdlib.h>
#include <string.h>

#include "protocol.h"
#include "transport.h"

#include "testutil.h"

// A stream over two byte buffers: one the peer "sent" (read), one this side
// writes into (checked afterwards).
typedef struct {
    const char *in;
    size_t in_length;
    size_t in_at;
    char *out;
    size_t out_length;
    size_t out_capacity;
} MemoryStream;

static size_t memory_read(void *context, char *buffer, size_t size)
{
    MemoryStream *m = (MemoryStream *)context;
    size_t left = m->in_length - m->in_at;
    size_t take = size < left ? size : left;
    memcpy(buffer, m->in + m->in_at, take);
    m->in_at += take;
    return take;
}

static bool memory_write(void *context, const char *bytes, size_t size)
{
    MemoryStream *m = (MemoryStream *)context;
    if (m->out_length + size + 1 > m->out_capacity) {
        size_t grown = (m->out_length + size + 1) * 2;
        m->out = (char *)realloc(m->out, grown);
        m->out_capacity = grown;
    }
    memcpy(m->out + m->out_length, bytes, size);
    m->out_length += size;
    m->out[m->out_length] = '\0';
    return true;
}

// Frames `body` the way a peer would, so read_message has a message to find.
static char *framed(const char *body, size_t *length)
{
    char *out = (char *)malloc(strlen(body) + 64);
    int header =
        sprintf(out, "Content-Length: %zu\r\n\r\n", strlen(body));
    memcpy(out + header, body, strlen(body));
    *length = header + strlen(body);
    return out;
}

static DapPeer peer_over(MemoryStream *m)
{
    DapPeer peer;
    peer.stream.context = m;
    peer.stream.read = memory_read;
    peer.stream.write = memory_write;
    peer.seq = 1;
    return peer;
}

static void test_read_request(void)
{
    LHAT_TEST("a framed request parses to its command and arguments");
    {
        size_t length = 0;
        char *message = framed(
            "{\"seq\":7,\"type\":\"request\",\"command\":\"setBreakpoints\","
            "\"arguments\":{\"line\":4}}",
            &length);
        MemoryStream m = {0};
        m.in = message;
        m.in_length = length;
        DapPeer peer = peer_over(&m);

        cJSON *request = dap_read(&peer);
        LHAT_CHECK(request != NULL, "the message parsed");
        LHAT_CHECK_EQ_STR(dap_command(request), strlen(dap_command(request)),
                          "setBreakpoints");
        const cJSON *arguments = dap_arguments(request);
        const cJSON *line = cJSON_GetObjectItem(arguments, "line");
        LHAT_CHECK(cJSON_IsNumber(line) && line->valuedouble == 4,
                   "the arguments came through");
        cJSON_Delete(request);
        free(message);
    }
}

// Reads the one message this side wrote back out of the memory buffer.
static cJSON *sent(MemoryStream *m)
{
    MemoryStream reader = {0};
    reader.in = m->out;
    reader.in_length = m->out_length;
    LhatStream stream;
    stream.context = &reader;
    stream.read = memory_read;
    stream.write = NULL;
    char *body = NULL;
    size_t length = 0;
    if (!lhat_transport_read_message(&stream, &body, &length)) {
        return NULL;
    }
    cJSON *parsed = cJSON_Parse(body);
    free(body);
    return parsed;
}

static void test_response_carries_request(void)
{
    LHAT_TEST("a response names the request's seq and command, framed");
    {
        size_t length = 0;
        char *message = framed(
            "{\"seq\":42,\"type\":\"request\",\"command\":\"threads\"}",
            &length);
        MemoryStream m = {0};
        m.in = message;
        m.in_length = length;
        DapPeer peer = peer_over(&m);

        cJSON *request = dap_read(&peer);
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "hi", "there");
        LHAT_CHECK(dap_respond(&peer, request, true, body), "it wrote");

        cJSON *response = sent(&m);
        LHAT_CHECK(response != NULL, "and it was framed to read back");
        const cJSON *type = cJSON_GetObjectItem(response, "type");
        const cJSON *request_seq = cJSON_GetObjectItem(response, "request_seq");
        const cJSON *command = cJSON_GetObjectItem(response, "command");
        const cJSON *success = cJSON_GetObjectItem(response, "success");
        LHAT_CHECK_EQ_STR(type->valuestring, strlen(type->valuestring),
                          "response");
        LHAT_CHECK(request_seq->valuedouble == 42, "the request's seq");
        LHAT_CHECK_EQ_STR(command->valuestring, strlen(command->valuestring),
                          "threads");
        LHAT_CHECK(cJSON_IsTrue(success), "success");

        cJSON_Delete(request);
        cJSON_Delete(response);
        free(message);
        free(m.out);
    }
}

static void test_event(void)
{
    LHAT_TEST("an event is framed with its name and body");
    {
        MemoryStream m = {0};
        DapPeer peer = peer_over(&m);
        cJSON *body = cJSON_CreateObject();
        cJSON_AddNumberToObject(body, "threadId", 1);
        LHAT_CHECK(dap_event(&peer, "stopped", body), "it wrote");

        cJSON *event = sent(&m);
        LHAT_CHECK(event != NULL, "and it framed");
        const cJSON *type = cJSON_GetObjectItem(event, "type");
        const cJSON *name = cJSON_GetObjectItem(event, "event");
        LHAT_CHECK_EQ_STR(type->valuestring, strlen(type->valuestring),
                          "event");
        LHAT_CHECK_EQ_STR(name->valuestring, strlen(name->valuestring),
                          "stopped");
        cJSON_Delete(event);
        free(m.out);
    }
}

static void test_two_messages_in_one_read(void)
{
    LHAT_TEST("two framed messages read one at a time, in order");
    {
        MemoryStream a = {0};
        char *first = framed("{\"command\":\"one\"}", &a.in_length);
        // Concatenate a second frame after the first.
        size_t second_length = 0;
        char *second = framed("{\"command\":\"two\"}", &second_length);
        char *both = (char *)malloc(a.in_length + second_length);
        memcpy(both, first, a.in_length);
        memcpy(both + a.in_length, second, second_length);
        a.in = both;
        a.in_length = a.in_length + second_length;
        DapPeer peer = peer_over(&a);

        cJSON *one = dap_read(&peer);
        cJSON *two = dap_read(&peer);
        LHAT_CHECK_EQ_STR(dap_command(one), strlen(dap_command(one)), "one");
        LHAT_CHECK_EQ_STR(dap_command(two), strlen(dap_command(two)), "two");
        cJSON_Delete(one);
        cJSON_Delete(two);
        free(first);
        free(second);
        free(both);
    }
}

int main(void)
{
    test_read_request();
    test_response_carries_request();
    test_event();
    test_two_messages_in_one_read();
    return lhat_test_report("test_dap_protocol");
}
