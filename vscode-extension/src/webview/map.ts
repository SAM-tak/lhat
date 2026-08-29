// L^ (lhat) -- the syntax tree, as lhat/ast answers with it, turned into an
// ELK graph. 06 の 5 章.
//
// Two axes and nothing else (5.1):
//   - the "voice" a node is read in decides the base direction --
//     statements run DOWN, expressions run RIGHT
//   - a branch's clauses TRANSPOSE that, and inside each clause the voice
//     goes back to what it was
// The voice changes in exactly two places: entering the expression a
// statement holds, and entering the body of a FUNC.
//
// Element lists (5.3.1) obey neither: they wrap into rows of a set width.

import type { AstNode, AstReply } from "../protocol.js";

const CH = 7.2; // mono advance at 12px
const LEAF_H = 30;
const HEAD_H = 24; // a container's label strip
const PAD = 10;
const MAX_LABEL = 48;
// The fold button sits inside the box. A container has a label strip wide
// enough to spare, but a folded one is only as wide as its own label, so it
// is widened by this much to keep the two off each other.
const FOLD_BTN = 18;

// 5.3's table, by kind.
const STATEMENT_LIST = new Set(["block"]);
// 04 の 4.5's try^{ } is here too: its items are if-clause nodes like an
// if^ statement's, the first being the body and the rest the catch arms. The
// body is not one of the alternatives, but it reads well enough at the left
// of them, and leaving try-block out of every table is what collapsed a
// whole try^{ } into a single leaf.
const BRANCH = new Set(["if-stmt", "if-expr", "try-block"]);
const ELEMENT_LIST = new Set([
    "table", "def", "self-table", "error-new", "errordef", "type-table",
]);
const BODY_STATEMENT = new Set(["for", "repeat", "with"]);
const VOICE_TURN = new Set(["func"]);

// V6: types are not drawn -- they ride along in the label of whatever they
// annotate. V5: the same for declaration lists.
const NOT_DRAWN = new Set([
    "type-name", "type-func", "type-coro", "type-table", "type-tuple",
    "type-union", "type-intersect", "param", "member-decl", "error-kind",
]);

// 5.6: the name a binding introduces and the condition a clause carries are
// what those constructs say, so they belong in the label rather than in a box.
// Drawing them is what made every definition read 'let^ … = …'.
const NOT_DRAWN_FIELDS: Record<string, string[]> = {
    define: ["targets"],
    reassign: ["targets"],
    "if-clause": ["condition"],
};

// A qualified name is a MEMBER tree, a box per dot if it is drawn.
const ALWAYS_LEAF = new Set(["module", "import-stmt", "require-stmt"]);

// V15: collapsed when a view is first opened. A collapsed container is a
// fixed-size leaf, so neither its size nor the layout's cost depends on what
// is inside it.
const COLLAPSIBLE = new Set(["func", "def", "self-table", "errordef"]);
// A definition of one of those is collapsed whole.
const FOLDS_WITH_VALUE = new Set(["define", "reassign"]);

// 5.3.1, V9 still open.
const COLUMNS: Record<string, number> = {
    table: 6, def: 1, "self-table": 1, errordef: 1, "type-table": 1,
    "error-new": 1,
};

export interface ElkNode {
    id: string;
    labels?: { text: string }[];
    width?: number;
    height?: number;
    x?: number;
    y?: number;
    children?: ElkNode[];
    edges?: ElkEdge[];
    layoutOptions?: Record<string, string>;
    /** Not ELK's: what this node was made from, for clicks and folding. */
    lhat?: {
        kind: string; start: number; end: number;
        collapsed?: boolean;
        /** Whether this one can be folded shut at all, open or not. */
        foldable?: boolean;
    };
}

export interface ElkEdge {
    id: string;
    sources: string[];
    targets: string[];
    /** 6.3's device for ELK, never drawn. */
    pinned?: boolean;
    sections?: {
        startPoint: { x: number; y: number };
        endPoint: { x: number; y: number };
        bendPoints?: { x: number; y: number }[];
    }[];
}

interface Child {
    field: string;
    node: AstNode;
    inList: boolean;
}

function allChildren(node: AstNode): Child[] {
    const out: Child[] = [];
    for (const [field, value] of Object.entries(node.fields ?? {})) {
        if (Array.isArray(value)) {
            for (const c of value) out.push({ field, node: c, inList: true });
        } else {
            out.push({ field, node: value, inList: false });
        }
    }
    return out.sort((a, b) => a.node.start - b.node.start);
}

function drawnChildren(node: AstNode): Child[] {
    if (ALWAYS_LEAF.has(node.kind)) return [];
    const skip = NOT_DRAWN_FIELDS[node.kind] ?? [];
    return allChildren(node).filter(
        (c) => !NOT_DRAWN.has(c.node.kind) && !skip.includes(c.field));
}

// 5.2: anything holding neither a branch nor a body is one box.
function holdsBranchOrBody(node: AstNode): boolean {
    for (const { node: c } of drawnChildren(node)) {
        if (BRANCH.has(c.kind) || VOICE_TURN.has(c.kind) ||
            holdsBranchOrBody(c)) {
            return true;
        }
    }
    return false;
}

function holdsCollapsible(node: AstNode): boolean {
    return drawnChildren(node).some(
        ({ node: c }) => COLLAPSIBLE.has(c.kind) || holdsCollapsible(c));
}

// V16: the construct's own text with every drawn child's span cut out, so a
// container shows what shapes it rather than everything below it. What is not
// drawn stays, which is how names and conditions reach the label.
function labelOf(node: AstNode, source: string, drawn: Child[]): string {
    const holes = drawn
        .map((c) => [c.node.start, c.node.end] as const)
        .sort((a, b) => a[0] - b[0]);
    let text = "";
    let cursor = node.start;
    for (const [start, end] of holes) {
        if (start > cursor) text += source.slice(cursor, start);
        if (end > cursor) {
            if (start > cursor || text !== "") text += "…";
            cursor = end;
        }
    }
    if (cursor < node.end) text += source.slice(cursor, node.end);

    text = text.replace(/\s+/g, " ").replace(/(…\s*)+/g, "… ").trim();
    if (text === "") text = node.kind;
    return text.length > MAX_LABEL ? text.slice(0, MAX_LABEL - 1) + "…" : text;
}

const widthFor = (label: string) =>
    Math.max(56, Math.round(label.length * CH) + 18);

// 06 の 8.2: a named subroutine is a unit of its own, so opening one means
// going into it rather than unfolding it where it stands. Anonymous ones are
// only meaningful where they were written, so they stay put.
export function isDrillTarget(node: AstNode): boolean {
    if (!COLLAPSIBLE.has(node.kind)) return false;
    return node.kind === "func" ? false : true;
}

/** The node a view is rooted at, found by the position it starts at. */
export function nodeAt(root: AstNode, start: number): AstNode | undefined {
    if (root.start === start) return root;
    for (const { node: c } of allChildren(root)) {
        if (start < c.start || start >= c.end) continue;
        const found = nodeAt(c, start);
        if (found !== undefined) return found;
    }
    return undefined;
}

/** What a definition is called, for the trail of where a view came from. */
export function titleOf(node: AstNode, source: string): string {
    return labelOf(node, source, drawnChildren(node)).replace(/\s*…\s*$/, "");
}

export interface MapOptions {
    /** V15: fold definitions when the view opens. */
    collapse?: boolean;
    /**
     * What the reader has folded or unfolded by hand, keyed by start
     * position. An entry decides that one node; everything without one
     * follows `collapse`. Positions rather than nodes, for the reason 8.2's
     * trail uses them: the tree is replaced whole on every edit.
     */
    folds?: Record<number, boolean>;
    /**
     * 8.2: the node this view is rooted at. Its own box is not drawn -- the
     * view *is* that definition -- so what shows is the body alone.
     */
    root?: AstNode;
}

export function toElk(reply: AstReply, options: MapOptions = {}): ElkNode {
    const source = reply.source;
    let counter = 0;
    const nextId = (kind: string) => `${kind}-${counter++}`;

    const from = (
        node: AstNode,
        extra: { collapsed?: boolean; foldable?: boolean } = {},
    ) => ({
        kind: node.kind, start: node.start, end: node.end, ...extra,
    });

    const leaf = (node: AstNode, label: string): ElkNode => ({
        id: nextId(node.kind),
        labels: [{ text: label }],
        width: widthFor(label),
        height: LEAF_H,
        lhat: from(node),
    });

    const chain = (id: string, kids: ElkNode[]): ElkEdge[] =>
        kids.slice(1).map((k, i) => ({
            id: `${id}__ord${i}`,
            sources: [kids[i].id],
            targets: [k.id],
            pinned: true,
        }));

    const container = (
        id: string, label: string, dir: string, children: ElkNode[],
        edges: ElkEdge[], node: AstNode, padded = true,
    ): ElkNode => ({
        id,
        labels: [{ text: label }],
        layoutOptions: {
            "elk.algorithm": "layered",
            // 6.2: never inherited, so always written.
            "elk.direction": dir,
            "elk.padding": padded
                ? `[top=${HEAD_H + PAD},left=${PAD},bottom=${PAD},right=${PAD}]`
                : "[top=0,left=0,bottom=0,right=0]",
            "elk.spacing.nodeNode": "14",
            "elk.layered.spacing.nodeNodeBetweenLayers": "20",
        },
        children,
        edges,
        lhat: from(node),
    });

    // 6.4: a list wraps into rows built here. rectpacking loses source order
    // once the elements differ in size, which an element list does.
    function griddedRows(id: string, kids: ElkNode[], columns: number): ElkNode[] {
        const rows: ElkNode[] = [];
        for (let i = 0; i < kids.length; i += columns) {
            const slice = kids.slice(i, i + columns);
            const rid = `${id}__r${rows.length}`;
            rows.push({
                id: rid,
                layoutOptions: {
                    "elk.algorithm": "layered",
                    "elk.direction": "RIGHT",
                    "elk.padding": "[top=0,left=0,bottom=0,right=0]",
                    "elk.spacing.nodeNode": "12",
                    "elk.layered.spacing.nodeNodeBetweenLayers": "12",
                },
                children: slice,
                edges: chain(rid, slice),
            });
        }
        return rows;
    }

    // `unfold` marks the way down to the body of the definition this view was
    // opened at (8.2). Going into a definition is what opens it, so neither it
    // nor its body is folded again -- but a definition *inside* that body is,
    // since that one is a way further in rather than part of what is shown.
    function build(node: AstNode, voice: "stmt" | "expr",
                   unfold: boolean): ElkNode {
        // Nothing with an empty body is worth a fold, so the emptiness is
        // asked about before anything else -- an f^() {} folded shut would
        // read '… …'.
        const foldable =
            (COLLAPSIBLE.has(node.kind) ||
                (FOLDS_WITH_VALUE.has(node.kind) && holdsCollapsible(node))) &&
            drawnChildren(node).length > 0;

        // A fold the reader made by hand outweighs the view-wide default,
        // which is what makes one definition openable inside a folded file
        // and one shuttable inside an open one.
        if (foldable &&
            (options.folds?.[node.start] ??
                (options.collapse === true && !unfold))) {
            const text = labelOf(node, source, drawnChildren(node)) + " …";
            return {
                id: nextId(node.kind),
                labels: [{ text }],
                // Room for the fold button, which sits inside the box.
                width: widthFor(text) + FOLD_BTN,
                height: LEAF_H + 8,
                lhat: from(node, { collapsed: true, foldable: true }),
            };
        }

        const built = expand(node, voice, unfold);
        // Said of an open one too: the button is how it gets shut again.
        if (foldable && built.lhat !== undefined) built.lhat.foldable = true;
        return built;
    }

    function expand(node: AstNode, voice: "stmt" | "expr",
                    unfold: boolean): ElkNode {
        const kind = node.kind;
        const kids = drawnChildren(node);
        const label = labelOf(node, source, kids);

        if (kids.length === 0) return leaf(node, label);

        // 5.2, statements included. The label is taken again with nothing cut
        // out: a leaf draws none of its children, so there are no holes to
        // leave. Cutting them here emptied the label of anything whose one
        // child covers the whole of it -- 'print(x)' read as "call-stmt".
        if (!STATEMENT_LIST.has(kind) && !BRANCH.has(kind) &&
            !ELEMENT_LIST.has(kind) && !VOICE_TURN.has(kind) &&
            !BODY_STATEMENT.has(kind) && !holdsBranchOrBody(node)) {
            return leaf(node, labelOf(node, source, []));
        }

        // Past a subroutine or definition, the way down has been walked: what
        // lies below is another way in rather than more of this view.
        const childUnfold = unfold && !COLLAPSIBLE.has(kind);

        const id = nextId(kind);

        // 5.3.1: an element list wraps, and does not follow the voice.
        if (ELEMENT_LIST.has(kind)) {
            const items = kids.map((c) => build(c.node, "expr", childUnfold));
            const columns = COLUMNS[kind] ?? 1;
            if (columns > 1 && items.length > columns) {
                const rows = griddedRows(id, items, columns);
                return container(id, label, "DOWN", rows, chain(id, rows), node);
            }
            return container(id, label, "DOWN", items, chain(id, items), node);
        }

        // A branch transposes; each clause goes back to the enclosing voice.
        if (BRANCH.has(kind)) {
            const clauses = kids.map((c) => build(c.node, voice, childUnfold));
            const dir = voice === "stmt" ? "RIGHT" : "DOWN";
            // 6.3: clauses carry no edge of their own, so ELK would pack them
            // by area and lose both the axis and the source order.
            return container(id, label, dir, clauses, chain(id, clauses), node);
        }

        const inner: ElkNode[] = [];
        for (const { field, node: c } of kids) {
            let childVoice = voice;
            if (voice === "stmt" && !STATEMENT_LIST.has(kind) &&
                !["body", "items", "extra"].includes(field)) {
                childVoice = "expr"; // a statement's expression
            }
            if (VOICE_TURN.has(kind) && field === "body") childVoice = "stmt";
            if (BODY_STATEMENT.has(kind) && field === "body") childVoice = "stmt";
            if (kind === "if-clause" && field === "body") childVoice = voice;
            inner.push(build(c, childVoice, childUnfold));
        }

        // V4: 9 章's clauses are `extra` on a BLOCK and run in a fixed order,
        // so they go down with the rest of the statements.
        const dir = voice === "stmt" ? "DOWN" : "RIGHT";
        return container(id, label, dir, inner, chain(id, inner), node);
    }

    const root = build(options.root ?? reply.root, "stmt",
                       options.root !== undefined);
    // The root of a view need not be a container -- one definition opened on
    // its own may map to a single box -- so give it something to sit in.
    if (!root.layoutOptions) {
        return {
            id: "view",
            layoutOptions: {
                "elk.algorithm": "layered",
                "elk.direction": "DOWN",
                "elk.padding": `[top=${PAD},left=${PAD},bottom=${PAD},right=${PAD}]`,
                "elk.spacing.nodeNode": "14",
            },
            children: [root],
            edges: [],
        };
    }
    root.layoutOptions["elk.padding"] =
        `[top=${PAD},left=${PAD},bottom=${PAD},right=${PAD}]`;
    root.labels = [];
    return root;
}
