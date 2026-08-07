// L^ (lhat) -- LSP server: file:// URIs and the paths program.h wants.
//
// program.c's normalise_path only unifies separators and folds '.'/'..' --
// it never turns an absolute path into a relative one, and resolve_against
// joins a require^'s relative text onto the last segment of whoever wrote
// it. So handing program.h absolute paths keeps a whole dependency graph
// absolute automatically, with no workspace-relative bookkeeping needed.
// This server settles on that: every path key -- LspDocumentStore,
// LhatUnit::path, the workspace's reverse index -- is this absolute,
// forward-slashed form. Client-visible positions travel as file:// URIs
// instead; these two functions are the only place that boundary is crossed.

#ifndef LSP_URI_H
#define LSP_URI_H

#include <stddef.h>

// file://... to an absolute path (forward-slashed; a Windows drive letter
// loses its leading '/' and is lowercased). NULL when the scheme is not
// file:. Percent-escapes are decoded. Malloc'd; the caller frees it.
char *lsp_uri_to_absolute_path(const char *uri);

// The reverse: an absolute path to a file:// URI, percent-encoded. Malloc'd.
char *lsp_absolute_path_to_uri(const char *absolute_path);

#endif  // LSP_URI_H
