// L^ (lhat) -- the graph webview on React Flow (06 の 8.4, the V14 spike).
//
// What this exists to prove or disprove:
//   - ELK owns every position; React Flow only shows them. Static nodes,
//     `parentId` subflows, ELK's parent-relative coordinates passed through
//   - parity with the SVG view: folding, drilling in, click-to-reveal
//   - V17: a container's contents can slide, and what an ancestor no longer
//     shows is cut -- React Flow's DOM is flat, so the cut is computed here
//   - V18: the slide is a drag on the container along the axis that carries
//     no order, without fighting the pane's own pan
//   - the connection UI for the decided data lines: drag from a handle
//
// The mapping (map.ts) is shared with the SVG view untouched, which is the
// point of keeping it framework-free.

import React, {
    useCallback, useEffect, useMemo, useRef, useState,
} from "react";
import { createRoot } from "react-dom/client";
import {
    Background, Controls, Handle, MiniMap, Position, ReactFlow, addEdge,
    useEdgesState, useReactFlow, ReactFlowProvider,
    type Connection, type Edge, type Node, type NodeProps, type NodeTypes,
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import "./rf.css";
import ELK from "elkjs/lib/elk.bundled.js";
import type { AstNode, AstReply, FromWebview, ToWebview } from "../../protocol";
import { nodeAt, titleOf, toElk, type ElkNode } from "../map";

declare function acquireVsCodeApi(): {
    postMessage(message: FromWebview): void;
    setState(state: unknown): void;
    getState(): unknown;
};

const vscode = acquireVsCodeApi();
const elk = new ELK();

// ---------------------------------------------------------------------------
// Laid-out ELK graph -> React Flow nodes

// V17/V18: how far a container's contents have been slid, keyed by the
// container's source position -- ELK ids are regenerated every layout, the
// source position is what survives one. The same key is what *.lhl would
// store (06 の 9 章).
type Slides = Record<number, { dx: number; dy: number }>;

interface BoxData extends Record<string, unknown> {
    label: string;
    depth: number;
    isContainer: boolean;
    collapsed: boolean;
    start?: number;
    end?: number;
    /**
     * V17's cut, applied to the box's own div rather than carried in
     * node.style: whatever is in node.style reaches the minimap's shapes
     * too, where a full-scale pixel inset would swallow the tiny rect.
     */
    clipPath?: string;
    /** Set when this container's contents may slide (it has a stable key). */
    slideKey?: number;
    /** V18: the axis that carries no order for this node -- see toFlow. */
    slideAxis: "x" | "y";
    onSlide: (key: number, dx: number, dy: number) => void;
    onAct: (data: BoxData) => void;
}

type BoxNodeType = Node<BoxData, "box">;

interface Rect { l: number; t: number; r: number; b: number }

function toFlow(
    laid: ElkNode,
    slides: Slides,
    onSlide: BoxData["onSlide"],
    onAct: BoxData["onAct"],
): BoxNodeType[] {
    const nodes: BoxNodeType[] = [];
    const dirOf = (n: ElkNode) => n.layoutOptions?.["elk.direction"] ?? "DOWN";

    const walk = (
        parent: ElkNode,
        parentId: string | undefined,
        parentAbs: { x: number; y: number },
        visible: Rect | undefined,
        depth: number,
    ): void => {
        const slide =
            (parent.lhat !== undefined ? slides[parent.lhat.start] : undefined) ??
            { dx: 0, dy: 0 };
        for (const c of parent.children ?? []) {
            const x = (c.x ?? 0) + slide.dx;
            const y = (c.y ?? 0) + slide.dy;
            const ax = parentAbs.x + x;
            const ay = parentAbs.y + y;
            const w = c.width ?? 0;
            const h = c.height ?? 0;

            // V17: what an ancestor no longer shows is cut, in this node's own
            // coordinates. React Flow keeps every node in one flat layer, so a
            // parent clips nothing by itself; the inset accumulates over every
            // ancestor instead.
            let clipPath: string | undefined;
            let hidden = false;
            if (visible !== undefined) {
                const cutL = Math.max(0, visible.l - ax);
                const cutT = Math.max(0, visible.t - ay);
                const cutR = Math.max(0, ax + w - visible.r);
                const cutB = Math.max(0, ay + h - visible.b);
                if (cutL + cutR >= w || cutT + cutB >= h) {
                    hidden = true;
                } else if (cutL || cutT || cutR || cutB) {
                    clipPath =
                        `inset(${cutT}px ${cutR}px ${cutB}px ${cutL}px)`;
                }
            }

            const isContainer = (c.children ?? []).length > 0;
            nodes.push({
                id: c.id,
                type: "box",
                position: { x, y },
                parentId,
                // As first-class fields, not style: the minimap decides
                // whether a node exists to draw by nodeHasDimensions(), which
                // reads these and never the style -- with them only in style,
                // the canvas measures its DOM and works while the minimap
                // draws nothing at all.
                width: w,
                height: h,
                draggable: false,
                // Not for selection itself: React Flow turns a node's
                // pointer-events off entirely when it is neither selectable
                // nor draggable and no node-level handlers are installed
                // (hasPointerEvents in NodeWrapper) -- which would kill our
                // own pointer handlers, the hover that shows the handles,
                // and every click. Selectable is the cheapest way to keep
                // events flowing.
                selectable: true,
                hidden,
                data: {
                    clipPath,
                    label: c.labels?.[0]?.text ?? "",
                    depth,
                    isContainer,
                    collapsed: c.lhat?.collapsed === true,
                    start: c.lhat?.start,
                    end: c.lhat?.end,
                    slideKey:
                        isContainer && c.lhat !== undefined
                            ? c.lhat.start : undefined,
                    // 8.3: the axis the parent stacks this node in carries the
                    // order, so it is dull; the cross axis slides. A child of
                    // a DOWN container slides horizontally, of a RIGHT one
                    // vertically.
                    slideAxis: dirOf(parent) === "RIGHT" ? "y" : "x",
                    onSlide,
                    onAct,
                },
            });

            if (isContainer && !hidden) {
                const own: Rect = { l: ax, t: ay, r: ax + w, b: ay + h };
                const next: Rect = visible === undefined ? own : {
                    l: Math.max(visible.l, own.l),
                    t: Math.max(visible.t, own.t),
                    r: Math.min(visible.r, own.r),
                    b: Math.min(visible.b, own.b),
                };
                walk(c, c.id, { x: ax, y: ay }, next, depth + 1);
            }
        }
    };

    walk(laid, undefined, { x: 0, y: 0 }, undefined, 1);
    return nodes;
}

// ---------------------------------------------------------------------------
// One node

function BoxNode({ data }: NodeProps<BoxNodeType>) {
    const { getZoom } = useReactFlow();
    const drag = useRef<{ x: number; y: number; moved: boolean } | null>(null);

    // V18, the immediate half: a drag on a container slides its contents on
    // the axis that carries no order. Pointer capture keeps the gesture ours;
    // stopPropagation keeps React Flow's pane from panning underneath it.
    const onPointerDown = (event: React.PointerEvent) => {
        event.stopPropagation();
        (event.target as Element).setPointerCapture(event.pointerId);
        drag.current = { x: event.clientX, y: event.clientY, moved: false };
    };
    const onPointerMove = (event: React.PointerEvent) => {
        const d = drag.current;
        if (d === null) return;
        const dx = event.clientX - d.x;
        const dy = event.clientY - d.y;
        if (!d.moved && Math.abs(dx) + Math.abs(dy) < 4) return;
        d.moved = true;
        d.x = event.clientX;
        d.y = event.clientY;
        if (data.slideKey === undefined) return;
        // Screen pixels over canvas zoom = graph units.
        const zoom = getZoom() || 1;
        if (data.slideAxis === "x") data.onSlide(data.slideKey, dx / zoom, 0);
        else data.onSlide(data.slideKey, 0, dy / zoom);
    };
    const onPointerUp = () => {
        const wasDrag = drag.current?.moved === true;
        drag.current = null;
        if (!wasDrag) data.onAct(data);
    };

    const classes = ["box"];
    if (data.collapsed) classes.push("folded");
    else if (data.isContainer) classes.push(`container d${Math.min(data.depth, 6)}`);
    else classes.push("leaf");
    // React Flow's pan is a native d3-zoom listener on the pane, which fires
    // during native bubbling -- long before React's delegated handlers, so a
    // stopPropagation here cannot reach it. Its zoom filter does honour this
    // class, and a slide must not drag the whole canvas along with it.
    if (data.slideKey !== undefined) classes.push("nopan");

    return (
        <>
            <div
                className={classes.join(" ")}
                style={data.clipPath ? { clipPath: data.clipPath } : undefined}
                onPointerDown={onPointerDown}
                onPointerMove={onPointerMove}
                onPointerUp={onPointerUp}
            >
                <div className="boxlabel">{data.label}</div>
            </div>
            {!data.isContainer && (
                <>
                    {/* The decided data lines (06 の 5.5) are drawn and edited
                        through handles like these; this pair is the mock that
                        tries the interaction out. Siblings of the box, not
                        children: the box clips its overflow and a handle sits
                        exactly on the edge. */}
                    <Handle type="target" position={Position.Left} />
                    <Handle type="source" position={Position.Right} />
                </>
            )}
        </>
    );
}

const nodeTypes: NodeTypes = { box: BoxNode };

// ---------------------------------------------------------------------------
// The app

function countNodes(n: ElkNode): number {
    let total = 1;
    for (const c of n.children ?? []) total += countNodes(c);
    return total;
}

function App() {
    const [reply, setReply] = useState<AstReply>();
    const [note, setNote] = useState("waiting for the language server…");
    const [collapse, setCollapse] = useState(true);
    const [trail, setTrail] = useState<number[]>([]);
    const [slides, setSlides] = useState<Slides>({});
    const [laid, setLaid] = useState<ElkNode>();
    const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([]);

    useEffect(() => {
        const onMessage = (event: MessageEvent<ToWebview>) => {
            const message = event.data;
            switch (message.type) {
                case "tree":
                    setReply(message.reply);
                    break;
                case "pending":
                    setNote("waiting for the language server…");
                    break;
                case "error":
                    setNote(message.message);
                    break;
            }
        };
        window.addEventListener("message", onMessage);
        vscode.postMessage({ type: "ready" });
        return () => window.removeEventListener("message", onMessage);
    }, []);

    // 8.2: the trail, resolved against the current tree -- positions rather
    // than nodes, so it survives the tree being replaced after an edit.
    const view = useMemo(() => {
        if (reply === undefined) return undefined;
        const path: AstNode[] = [];
        let root = reply.root;
        for (const start of trail) {
            const found = nodeAt(reply.root, start);
            if (found === undefined) break;
            path.push(found);
            root = found;
        }
        return { root, path };
    }, [reply, trail]);

    useEffect(() => {
        if (reply === undefined || view === undefined) return;
        let stale = false;
        const graph = toElk(reply, {
            collapse,
            root: view.path.length > 0 ? view.root : undefined,
        });
        const started = performance.now();
        void elk.layout(graph).then((result) => {
            if (stale) return;
            const done = result as ElkNode;
            setLaid(done);
            setNote(`${countNodes(done)} nodes, ` +
                `${Math.round(performance.now() - started)}ms` +
                (collapse ? ", definitions folded" : ""));
        });
        return () => { stale = true; };
    }, [reply, view, collapse]);

    const onSlide = useCallback((key: number, dx: number, dy: number) => {
        setSlides((s) => ({
            ...s,
            [key]: { dx: (s[key]?.dx ?? 0) + dx, dy: (s[key]?.dy ?? 0) + dy },
        }));
    }, []);

    const onAct = useCallback((data: BoxData) => {
        if (reply === undefined || data.start === undefined) return;
        if (data.collapsed) {
            const node = nodeAt(reply.root, data.start);
            if (node !== undefined) {
                const start = data.start;
                setTrail((t) => [...t, start]);
                return;
            }
        }
        if (data.end !== undefined) {
            vscode.postMessage({
                type: "reveal", start: data.start, end: data.end,
            });
        }
    }, [reply]);

    const nodes = useMemo(
        () => (laid !== undefined ? toFlow(laid, slides, onSlide, onAct) : []),
        [laid, slides, onSlide, onAct]);

    const onConnect = useCallback((connection: Connection) => {
        setEdges((current) => addEdge({ ...connection, animated: true }, current));
    }, [setEdges]);

    return (
        <div id="app">
            <div id="bar">
                <button
                    type="button"
                    title="Leave this definition"
                    disabled={trail.length === 0}
                    onClick={() => setTrail((t) => t.slice(0, -1))}
                >▲</button>
                <button type="button" onClick={() => setCollapse((v) => !v)}>
                    fold / unfold
                </button>
                <span id="status">{note}</span>
            </div>
            {view !== undefined && view.path.length > 0 && reply !== undefined && (
                <div id="trail">
                    <button type="button" className="crumb"
                        onClick={() => setTrail([])}>(file)</button>
                    {view.path.map((step, index) => (
                        <React.Fragment key={step.start}>
                            <span className="sep">›</span>
                            <button type="button" className="crumb"
                                onClick={() => setTrail(trail.slice(0, index + 1))}>
                                {titleOf(step, reply.source)}
                            </button>
                        </React.Fragment>
                    ))}
                </div>
            )}
            <div id="flow">
                <ReactFlow
                    // Remounting on a new view or fold state is what refits the
                    // viewport; at ≤25 nodes a remount costs nothing visible.
                    key={trail.join(",") + (collapse ? "|c" : "|o")}
                    nodes={nodes}
                    edges={edges}
                    onEdgesChange={onEdgesChange}
                    onConnect={onConnect}
                    nodeTypes={nodeTypes}
                    fitView
                    minZoom={0.05}
                >
                    <Background />
                    <MiniMap pannable zoomable />
                    <Controls />
                </ReactFlow>
            </div>
        </div>
    );
}

const root = document.getElementById("root");
if (root !== null) {
    createRoot(root).render(
        <ReactFlowProvider>
            <App />
        </ReactFlowProvider>,
    );
}
