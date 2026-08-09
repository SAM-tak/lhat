// L^ (lhat) -- what the extension host and the graph webview say to each
// other, and the shape lhatls answers lhat/ast with.
//
// 06 の 4.1 defines the reply; this file only restates it as types. The
// webview cannot speak LSP itself (it has no socket to lhatls and no language
// client), so the host asks on its behalf and forwards the answer -- 07 の L3.

/** One node of the syntax tree, as lhat/ast writes it (06 の 4.1). */
export interface AstNode {
    kind: string;
    /** UTF-16 code unit offsets into `AstReply.source`, half-open. */
    start: number;
    end: number;
    line: number;
    column: number;
    comments?: AstComment[];
    /**
     * Children, keyed by the union member they came from in ast.h. A member
     * holding a list is an array even when it holds one.
     */
    fields?: Record<string, AstNode | AstNode[]>;
}

export interface AstComment {
    start: number;
    end: number;
    block: boolean;
}

export interface AstReply {
    /** The whole unit. Every position above indexes into this. */
    source: string;
    root: AstNode;
}

/** Host -> webview. */
export type ToWebview =
    | { type: "tree"; reply: AstReply; uri: string }
    /** The unit is not part of any checked root yet (06 の 4.3). */
    | { type: "pending" }
    | { type: "error"; message: string };

/** Webview -> host. */
export type FromWebview =
    | { type: "ready" }
    /** Ask for the tree again -- after an edit, or after "pending". */
    | { type: "refresh" }
    /** Put the text cursor on what was clicked in the graph. */
    | { type: "reveal"; start: number; end: number };
