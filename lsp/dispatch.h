// L^ (lhat) -- LSP server: method name -> handler, table-driven.
//
// Adding a request or notification (hover, definition, ...) later means
// adding one entry to dispatch_table.c and one handlers/*.c -- nothing here
// or in server.c changes.

#ifndef LSP_DISPATCH_H
#define LSP_DISPATCH_H

#include <stddef.h>

#include "cJSON.h"

typedef struct LspServer LspServer;

// `params` borrows the request's "params" member (may be NULL). A request
// handler returns the owned "result" value; a notification handler returns
// nothing because LSP notifications never get one.
typedef cJSON *(*LspRequestHandler)(LspServer *server, const cJSON *params);
typedef void (*LspNotificationHandler)(LspServer *server, const cJSON *params);

typedef struct {
    const char *method;  // NULL ends the table
    LspRequestHandler handler;
} LspRequestEntry;

typedef struct {
    const char *method;  // NULL ends the table
    LspNotificationHandler handler;
} LspNotificationEntry;

// Defined in dispatch_table.c, NULL-terminated.
extern const LspRequestEntry LSP_REQUEST_TABLE[];
extern const LspNotificationEntry LSP_NOTIFICATION_TABLE[];

// Parses `body` as one JSON-RPC message and routes it. A request whose
// method is not in LSP_REQUEST_TABLE gets a -32601 error response; an
// unrecognised notification is dropped, per the base protocol.
void lsp_dispatch_message(LspServer *server, const char *body, size_t length);

#endif  // LSP_DISPATCH_H
