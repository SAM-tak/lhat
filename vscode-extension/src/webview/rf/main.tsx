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
    Background, Handle, MarkerType, MiniMap, PanOnScrollMode, Position,
    ReactFlow, addEdge, useEdgesState, useReactFlow, ReactFlowProvider,
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

// V17/V18: how far a container's contents have been slid, keyed by what the
// container was made from -- ELK ids are regenerated every layout, the source
// span survives one. The same key is what *.lhl would store (06 の 9 章).
//
// The start alone will not do: a parent and its first child begin at the same
// place all the time (the root and the first statement of a file, for one), so
// a key of position alone is shared down a chain of containers and every one
// of them adds the same slide again -- the contents drift further the deeper
// they sit. Span and kind together are unique, since two nodes of one kind
// covering exactly one range would be the same node.
type Slides = Record<string, { dx: number; dy: number }>;

const slideKeyOf = (lhat: NonNullable<ElkNode["lhat"]>) =>
    `${lhat.kind}:${lhat.start}:${lhat.end}`;

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
    slideKey?: string;
    /** V18: the axis that carries no order for this node -- see toFlow. */
    slideAxis: "x" | "y";
    onSlide: (key: string, dx: number, dy: number) => void;
    /**
     * 8.6: a branch's clauses snap. One slide offset per clause, at which
     * that clause's centre sits on the container's own axis -- the position
     * where the execution line through it runs straight.
     */
    snapStops?: number[];
    /** The container's current slide, for the dots and for snapping. */
    slideDx: number;
    /**
     * 8.6: the range within which the execution line through this box stays
     * vertical -- the same span the handle clamp covers. Outside it the drag
     * meets resistance and the release springs back. Only for x slides.
     */
    slideMin?: number;
    slideMax?: number;
    /** Set the slide outright (a snap), with the short glide the CSS gives. */
    onSnap: (key: string, dx: number) => void;
    /**
     * 8.6: where the execution line crosses this box, in its own pixels.
     * Undefined means the middle. A base-left shift moves the box, not the
     * line: the handles stay on the chain's axis -- where the centre was
     * before the shift -- so the line stays vertical and only the box slid.
     */
    flowHandleX?: number;
    /** Left click: go into a folded definition. Nothing otherwise. */
    onEnter: (data: BoxData) => void;
    /** Middle click: show what this was made from, in the text. */
    onReveal: (data: BoxData) => void;
    /** Whether this one can be folded shut, which is what shows the button. */
    foldable: boolean;
    /** The button: fold this one node, or open it, whatever the bar says. */
    onFold: (data: BoxData) => void;
}

type BoxNodeType = Node<BoxData, "box">;

interface Rect { l: number; t: number; r: number; b: number }

function toFlow(
    laid: ElkNode,
    slides: Slides,
    viewWidth: number,
    onSlide: BoxData["onSlide"],
    onSnap: BoxData["onSnap"],
    onEnter: BoxData["onEnter"],
    onReveal: BoxData["onReveal"],
    onFold: BoxData["onFold"],
): { nodes: BoxNodeType[]; exec: Edge[] } {
    const nodes: BoxNodeType[] = [];
    // 8.6: the execution lines. The layout's own order-pinning edges (6.3),
    // shown where the mapping marked them -- the statement sequences.
    const exec: Edge[] = [];
    const dirOf = (n: ElkNode) => n.layoutOptions?.["elk.direction"] ?? "DOWN";

    // 8.6: the document's axis is the vertical centre line the viewport is
    // centred on. Anything longer than the view cannot be centred -- its
    // start would fall off the left, which is the wrong end to lose -- so it
    // hangs from the base left edge instead: shifted right, display-only,
    // until its left edge sits where the view's left margin is. Computed
    // before the slide is added, so a deliberate partial scroll still moves
    // it off that alignment.
    const usable = viewWidth - 16;
    const baseAbs = (laid.width ?? 0) / 2 - usable / 2;

    const walk = (
        parent: ElkNode,
        parentId: string | undefined,
        parentAbs: { x: number; y: number },
        visible: Rect | undefined,
        depth: number,
        // The ancestors' slides added up. The base-left landing is judged
        // against the position *without* them: a slid ancestor is the reader
        // scrolling this subtree, and a child that re-anchored itself to the
        // base edge on every frame would stick to the screen while its
        // parent moved away.
        slidX: number,
    ): void => {
        // Edges join siblings, so what an edge needs to know about hiding is
        // settled within this one level.
        const hiddenIds = new Set<string>();
        for (const c of parent.children ?? []) {
            let x = c.x ?? 0;
            let baseShift = 0;
            if (usable > 0 && (c.width ?? 0) > usable) {
                const shift = baseAbs - (parentAbs.x - slidX + x);
                if (shift > 0) {
                    x += shift;
                    baseShift = shift;
                }
            }
            // 8.3改: the slide moves the node itself -- grab a box and the
            // whole box goes, frame and all. Its children ride along for
            // free: they are positioned relative to it.
            //
            // A box that fits the view whole has nothing to scroll to, so it
            // neither slides nor snaps: its stored offset -- kept from when
            // it was wider, folded shut being the usual way -- is ignored
            // rather than deleted, and comes back to life when unfolding
            // makes the box wide again.
            const fitsX = usable > 0 && (c.width ?? 0) <= usable;
            const own = c.lhat !== undefined
                ? slides[slideKeyOf(c.lhat)] : undefined;
            const ownDx = fitsX ? 0 : own?.dx ?? 0;
            x += ownDx;
            // Horizontal only. Vertical is the document's own axis -- the
            // global scroll already covers what sticks out up or down, so a
            // per-node vertical slide would be a second way to do the same
            // thing, and one that bends the execution line for nothing.
            const y = c.y ?? 0;
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
            if (hidden) hiddenIds.add(c.id);
            // dx per clause that slides the box until that clause's centre
            // sits under the execution line -- which stays put on the chain's
            // axis (flowHandleX) while the box moves beneath it.
            const snapStops =
                !fitsX &&
                c.lhat?.branch === true && (c.children?.length ?? 0) > 1
                    ? (c.children ?? []).map((k) =>
                        (c.width ?? 0) / 2 - baseShift
                            - ((k.x ?? 0) + (k.width ?? 0) / 2))
                    : undefined;
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
                        isContainer && c.lhat !== undefined &&
                            dirOf(parent) !== "RIGHT" && !fitsX
                            ? slideKeyOf(c.lhat) : undefined,
                    // 8.3: the axis the parent stacks this node in carries the
                    // order, so it is dull; the cross axis slides. A child of
                    // a DOWN container slides horizontally, of a RIGHT one
                    // vertically.
                    slideAxis: dirOf(parent) === "RIGHT" ? "y" : "x",
                    snapStops,
                    slideDx: ownDx,
                    slideMin: isContainer && dirOf(parent) !== "RIGHT"
                        ? (c.width ?? 0) / 2 - baseShift - (c.width ?? 0) + 6
                        : undefined,
                    slideMax: isContainer && dirOf(parent) !== "RIGHT"
                        ? (c.width ?? 0) / 2 - baseShift - 6
                        : undefined,
                    foldable: c.lhat?.foldable === true,
                    // The line does not follow the box: the handle counters
                    // both the base-left landing and the reader's own slide,
                    // staying on the chain's axis (clamped to the box, so a
                    // slide past the axis bends the line rather than
                    // detaching it).
                    flowHandleX: baseShift > 0 || ownDx !== 0
                        ? Math.min(Math.max(
                            (c.width ?? 0) / 2 - baseShift - ownDx, 6),
                            (c.width ?? 0) - 6)
                        : undefined,
                    onSlide,
                    onSnap,
                    onEnter,
                    onReveal,
                    onFold,
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
                walk(c, c.id, { x: ax, y: ay }, next, depth + 1,
                     slidX + ownDx);
            }
        }
        for (const e of parent.edges ?? []) {
            if (e.drawn !== true) continue;
            if (hiddenIds.has(e.sources[0]) || hiddenIds.has(e.targets[0])) {
                continue;
            }
            exec.push({
                id: `x__${e.id}`,
                source: e.sources[0],
                target: e.targets[0],
                sourceHandle: "flow-out",
                targetHandle: "flow-in",
                type: "straight",
                className: "exec",
                selectable: false,
                focusable: false,
                markerEnd: {
                    type: MarkerType.ArrowClosed,
                    width: 11,
                    height: 11,
                    color: "var(--lhat-exec)",
                },
            });
        }
    };

    walk(laid, undefined, { x: 0, y: 0 }, undefined, 1, 0);
    return { nodes, exec };
}

/**
 * 8.6: inertia. Called with the release velocity (px/ms); keeps stepping
 * with exponential decay (0.998 per ms -- iOS's "normal" rate) until the
 * motion is too small to see. Returns the cancel, for the next touch.
 */
function fling(
    velocity: number, step: (d: number) => boolean | void,
): () => void {
    let last = performance.now();
    let vel = velocity;
    let raf = 0;
    const tick = (now: number) => {
        const dt = now - last;
        last = now;
        vel *= Math.pow(0.998, dt);
        if (Math.abs(vel) < 0.02) return;
        // false from the step ends the flight -- it hit a rubber band.
        if (step(vel * dt) === false) return;
        raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
}

/** The rubber band's return: an ease toward the bound, cancellable. */
function springTo(
    read: () => number, target: number, apply: (v: number) => void,
): () => void {
    let raf = 0;
    const tick = () => {
        const cur = read();
        const next = cur + (target - cur) * 0.18;
        if (Math.abs(target - next) < 0.5) {
            apply(target);
            return;
        }
        apply(next);
        raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
}

type Sample = { t: number; p: number };

/** Velocity in px/ms over the last stretch of samples, 0 if too brief. */
function releaseVelocity(samples: Sample[]): number {
    const first = samples[0];
    const last = samples[samples.length - 1];
    if (last === undefined || first === undefined) return 0;
    const dt = last.t - first.t;
    return dt > 20 ? (last.p - first.p) / dt : 0;
}

const trimSamples = (samples: Sample[], now: number): void => {
    while (samples.length > 1 && now - samples[0].t > 120) samples.shift();
};

/** Which snap stop the current slide is closest to. */
function nearestStop(stops: number[], dx: number): number {
    let best = 0;
    for (let i = 1; i < stops.length; i++) {
        if (Math.abs(stops[i] - dx) < Math.abs(stops[best] - dx)) best = i;
    }
    return best;
}

/**
 * For every button here. A press with the pointer leaves the button focused,
 * and the browser then shows its focus ring at the next keystroke -- which,
 * over a graph where Shift is React Flow's multi-select, means a ring appears
 * around a button nobody is using and reads as some mode having been entered.
 *
 * Keeping mousedown's default off stops the focus from moving at all. The
 * click still fires. A button reached by Tab is untouched: it takes the focus
 * and keeps the ring, which is who the ring is for.
 */
const keepFocusOff = (event: React.MouseEvent) => event.preventDefault();

// ---------------------------------------------------------------------------
// One node

function BoxNode({ data }: NodeProps<BoxNodeType>) {
    const { getZoom } = useReactFlow();
    const drag = useRef<{
        x: number; y: number; moved: boolean; samples: Sample[];
    } | null>(null);
    const flingStop = useRef<(() => void) | null>(null);

    // V18, the immediate half: a drag on a container slides its contents on
    // the axis that carries no order. Pointer capture keeps the gesture ours.
    const onPointerDown = (event: React.PointerEvent) => {
        // Middle press: the browser would start its own autoscroll here, and
        // the click that follows is what shows the text.
        if (event.button === 1) {
            event.preventDefault();
            return;
        }
        // The left button only. The middle one used to slide the contents as
        // well, which put a slide and a scroll on the same gesture.
        if (event.button !== 0) return;
        event.stopPropagation();
        flingStop.current?.();
        flingStop.current = null;
        (event.target as Element).setPointerCapture(event.pointerId);
        drag.current = {
            x: event.clientX, y: event.clientY, moved: false,
            samples: [{
                t: performance.now(),
                p: data.slideAxis === "x" ? event.clientX : event.clientY,
            }],
        };
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
        const now = performance.now();
        d.samples.push({
            t: now,
            p: data.slideAxis === "x" ? event.clientX : event.clientY,
        });
        trimSamples(d.samples, now);
        // Screen pixels over canvas zoom = graph units.
        const zoom = getZoom() || 1;
        let step = (data.slideAxis === "x" ? dx : dy) / zoom;
        // Past the range the line can stay vertical in, the drag pulls
        // against the band -- a third of the movement, so the edge is felt.
        if (data.slideMin !== undefined && data.slideMax !== undefined &&
            ((data.slideDx > data.slideMax && step > 0) ||
                (data.slideDx < data.slideMin && step < 0))) {
            step /= 3;
        }
        if (data.slideAxis === "x") data.onSlide(data.slideKey, step, 0);
        else data.onSlide(data.slideKey, 0, step);
    };
    const onPointerUp = (event: React.PointerEvent) => {
        if (event.button !== 0 || drag.current === null) return;
        const dragged = drag.current;
        const wasDrag = dragged.moved;
        drag.current = null;
        // A press that never moved was a click on the box, not a slide.
        if (!wasDrag) {
            data.onEnter(data);
            return;
        }
        if (data.slideKey === undefined) return;
        const key = data.slideKey;
        const velocity = releaseVelocity(dragged.samples);
        // 8.6: a branch slides freely under the finger; let go, the throw is
        // carried to where it would land and the nearest clause there takes
        // it -- a fling turns the page.
        if (data.snapStops !== undefined) {
            const landing = data.slideDx + velocity * 320;
            const at = nearestStop(data.snapStops, landing);
            data.onSnap(key, data.snapStops[at]);
            return;
        }
        const min = data.slideMin;
        const max = data.slideMax;
        // Let go outside the band and it comes home -- onSnap's glide is the
        // spring.
        if (min !== undefined && max !== undefined &&
            (data.slideDx < min || data.slideDx > max)) {
            data.onSnap(key, Math.min(Math.max(data.slideDx, min), max));
            return;
        }
        // Everything else keeps its momentum and glides out, until the band
        // catches it.
        if (Math.abs(velocity) > 0.05) {
            let acc = data.slideDx;
            flingStop.current = fling(velocity, (d) => {
                acc += d;
                if (min !== undefined && max !== undefined &&
                    (acc < min || acc > max)) {
                    data.onSnap(key, Math.min(Math.max(acc, min), max));
                    return false;
                }
                if (data.slideAxis === "x") data.onSlide(key, d, 0);
                else data.onSlide(key, 0, d);
            });
        }
    };

    // Showing the text is the middle button's. On the left it kept firing
    // when a slide or a connection was what was meant -- the gestures start
    // the same way, and only the one that turns out not to be a drag can be
    // told apart, by which time the text has already been jumped to.
    const onAuxClick = (event: React.MouseEvent) => {
        if (event.button !== 1) return;
        event.preventDefault();
        data.onReveal(data);
    };

    const classes = ["box"];
    if (data.collapsed) classes.push("folded");
    else if (data.isContainer) classes.push(`container d${Math.min(data.depth, 6)}`);
    else classes.push("leaf");
    // No `nopan` here. It was what kept a slide from dragging the canvas with
    // it, back when a drag could pan; with panOnDrag off there is nothing left
    // to hold back -- and the class would cost us, since inside one React Flow
    // stops the wheel from scrolling too.

    return (
        <>
            <div
                className={classes.join(" ")}
                style={data.clipPath ? { clipPath: data.clipPath } : undefined}
                onPointerDown={onPointerDown}
                onPointerMove={onPointerMove}
                onPointerUp={onPointerUp}
                onAuxClick={onAuxClick}
            >
                {data.foldable && (
                    // Its own gestures, kept off the box's: a press here must
                    // not start a slide, and the click must not be read as
                    // going into the definition.
                    <button
                        type="button"
                        className="foldbtn"
                        title={data.collapsed
                            ? "Unfold this definition"
                            : "Fold this definition shut"}
                        onMouseDown={keepFocusOff}
                        onPointerDown={(event) => event.stopPropagation()}
                        onClick={(event) => {
                            event.stopPropagation();
                            data.onFold(data);
                        }}
                    >{data.collapsed ? "▸" : "▾"}</button>
                )}
                <div className="boxlabel">{data.label}</div>
                {data.snapStops !== undefined && (
                    // 8.6: one dot per clause, the settled one lit. Their own
                    // gestures, like the fold button's: a press must not start
                    // a slide and the click must not read as entering. They
                    // sit where the execution line does -- on the chain's
                    // axis, countering the box's slide the way the handles
                    // do -- so they hold still on screen while the box moves.
                    <div
                        className="clausedots"
                        style={data.flowHandleX !== undefined
                            ? { left: data.flowHandleX } : undefined}
                    >
                        {data.snapStops.map((stop, i) => (
                            <button
                                key={i}
                                type="button"
                                className={
                                    i === nearestStop(
                                        data.snapStops ?? [], data.slideDx)
                                        ? "clausedot lit" : "clausedot"}
                                title={`Clause ${i + 1}`}
                                onMouseDown={keepFocusOff}
                                onPointerDown={(ev) => ev.stopPropagation()}
                                onClick={(ev) => {
                                    ev.stopPropagation();
                                    if (data.slideKey !== undefined) {
                                        data.onSnap(data.slideKey, stop);
                                    }
                                }}
                            />
                        ))}
                    </div>
                )}
            </div>
            {/* 8.6: where the execution lines fasten. Never shown, never a
                place to start a connection -- the arrows are the picture's,
                not the reader's. flowHandleX keeps them on the chain's axis
                when the box itself was shifted to the base left edge. */}
            <Handle type="target" position={Position.Top} id="flow-in"
                    isConnectable={false} className="flowhandle"
                    style={data.flowHandleX !== undefined
                        ? { left: data.flowHandleX } : undefined} />
            <Handle type="source" position={Position.Bottom} id="flow-out"
                    isConnectable={false} className="flowhandle"
                    style={data.flowHandleX !== undefined
                        ? { left: data.flowHandleX } : undefined} />
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

/**
 * How many of what this view shows are folded shut. Read off the laid-out
 * graph rather than kept alongside it: the state is the picture, and a flag
 * held next to it would have to be right about the default, every fold set by
 * hand, and what drilling in leaves out of the view. A folded node has no
 * children in the graph, so what is inside one is not counted -- which is
 * what makes this "shown", not "in the file".
 */
function countFolded(n: ElkNode): number {
    let total = n.lhat?.collapsed === true ? 1 : 0;
    for (const c of n.children ?? []) total += countFolded(c);
    return total;
}

function App() {
    const { setViewport, getViewport } = useReactFlow();
    const [reply, setReply] = useState<AstReply>();
    const [note, setNote] = useState("waiting for the language server…");
    // V15: what a node with nothing said about it does. Not the state of the
    // bar's button -- that is read off the graph (countFolded) -- and not
    // something to reason from: after one press of the button and a few of the
    // node's own, this alone says nothing about what is on screen.
    const [foldByDefault, setFoldByDefault] = useState(true);
    // What the reader folded or unfolded one at a time, over that default.
    const [folds, setFolds] = useState<Record<number, boolean>>({});
    const [trail, setTrail] = useState<number[]>([]);
    const [slides, setSlides] = useState<Slides>({});
    const [laid, setLaid] = useState<ElkNode>();
    const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([]);
    // 8.6: zoom is the type size. The scale everything else derives from it.
    const [fontPx, setFontPx] = useState(12);
    const scale = fontPx / 12;
    // 8.6: the width the view has -- a ceiling for wrapping, re-measured on
    // resize. Zero until first measured; nothing lays out before that.
    const [viewWidth, setViewWidth] = useState(0);
    const [viewHeight, setViewHeight] = useState(0);
    const flowRef = useRef<HTMLDivElement | null>(null);
    // Briefly on after a snap, so the settling glides instead of jumping.
    const [snapAnim, setSnapAnim] = useState(false);
    const snapTimer = useRef<number | undefined>(undefined);

    useEffect(() => {
        const el = flowRef.current;
        if (el === null) return;
        let timer: number | undefined;
        const ro = new ResizeObserver(() => {
            window.clearTimeout(timer);
            timer = window.setTimeout(() => {
                setViewWidth(el.clientWidth);
                setViewHeight(el.clientHeight);
            }, 150);
        });
        ro.observe(el);
        setViewWidth(el.clientWidth);
        setViewHeight(el.clientHeight);
        return () => { window.clearTimeout(timer); ro.disconnect(); };
    }, []);

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
        if (viewWidth === 0) return;
        let stale = false;
        const graph = toElk(reply, {
            collapse: foldByDefault,
            folds,
            root: view.path.length > 0 ? view.root : undefined,
            scale,
            width: viewWidth - 16,
        });
        const started = performance.now();
        void elk.layout(graph).then((result) => {
            if (stale) return;
            const done = result as ElkNode;
            setLaid(done);
            const folded = countFolded(done);
            setNote(`${countNodes(done)} nodes, ` +
                `${Math.round(performance.now() - started)}ms` +
                (folded > 0 ? `, ${folded} folded` : ""));
        });
        return () => { stale = true; };
    }, [reply, view, foldByDefault, folds, scale, viewWidth]);

    // What the bar's button says and does, both from the picture itself. One
    // definition still folded is enough to make the press an unfold: the way
    // out of a half-open view is a single press, whichever half it is in.
    const folded = useMemo(
        () => (laid === undefined ? 0 : countFolded(laid)), [laid]);

    // 8.6: there is no fitView in a document, and no free horizontal
    // position either. The view's x always holds the document's axis -- the
    // vertical centre line the execution line runs down -- at the middle of
    // the screen, re-derived from every layout, so a re-layout at another
    // type size cannot drift the picture sideways. Unclamped: with a
    // definition wider than the view the x goes negative, and that is right
    // -- the wide box hangs from the base left edge (toFlow) while
    // everything narrow stays centred.
    //
    // y is the one axis the reader owns. It resets to the top only when the
    // view is a different thing to look at (another definition, Fold/Unfold
    // All); a re-layout in place keeps it.
    const place = useRef(true);
    useEffect(() => {
        if (laid === undefined) return;
        const w = flowRef.current?.clientWidth ?? 0;
        const gw = laid.width ?? 0;
        const y = place.current ? 8 : getViewport().y;
        place.current = false;
        setViewport({ x: (w - gw) / 2, y, zoom: 1 });
    }, [laid, viewWidth, setViewport, getViewport]);

    const onSlide = useCallback((key: string, dx: number, dy: number) => {
        setSlides((s) => ({
            ...s,
            [key]: { dx: (s[key]?.dx ?? 0) + dx, dy: (s[key]?.dy ?? 0) + dy },
        }));
    }, []);

    // A snap sets the slide outright and lets the transition carry it there.
    const onSnap = useCallback((key: string, dx: number) => {
        setSlides((s) => ({ ...s, [key]: { dx, dy: 0 } }));
        setSnapAnim(true);
        window.clearTimeout(snapTimer.current);
        snapTimer.current = window.setTimeout(
            () => setSnapAnim(false), 200);
    }, []);
    const onSnapRef = useRef(onSnap);
    onSnapRef.current = onSnap;
    const onSlideRef = useRef(onSlide);
    onSlideRef.current = onSlide;

    // 8.2: a folded definition is a way in. Anything else does nothing on the
    // left button, which is what leaves it free for sliding and connecting.
    const onEnter = useCallback((data: BoxData) => {
        if (reply === undefined || data.start === undefined) return;
        if (!data.collapsed) return;
        const start = data.start;
        if (nodeAt(reply.root, start) !== undefined) {
            setTrail((t) => [...t, start]);
        }
    }, [reply]);

    const onReveal = useCallback((data: BoxData) => {
        if (data.start === undefined || data.end === undefined) return;
        vscode.postMessage({
            type: "reveal", start: data.start, end: data.end,
        });
    }, []);

    // One node's own fold. Written down rather than toggled in place: what it
    // is now comes from the bar's default as often as from an earlier press,
    // so the entry records the state asked for, not a flip of one held here.
    const onFold = useCallback((data: BoxData) => {
        if (data.start === undefined) return;
        const start = data.start;
        setFolds((f) => ({ ...f, [start]: !data.collapsed }));
    }, []);

    const flow = useMemo(
        () => (laid !== undefined
            ? toFlow(laid, slides, viewWidth,
                     onSlide, onSnap, onEnter, onReveal, onFold)
            : { nodes: [], exec: [] }),
        [laid, slides, viewWidth, onSlide, onSnap, onEnter, onReveal, onFold]);
    const nodes = flow.nodes;

    // What the wheel needs to know about the node under the pointer, by node
    // id. A ref because the wheel listener is native (below) and must not be
    // re-installed per render.
    const slidables = useMemo(() => {
        const m = new Map<string, {
            key: string; axis: "x" | "y"; stops?: number[]; dx: number;
        }>();
        for (const n of nodes) {
            const d = n.data;
            if (d.slideKey !== undefined) {
                m.set(n.id, {
                    key: d.slideKey, axis: d.slideAxis,
                    stops: d.snapStops, dx: d.slideDx,
                });
            }
        }
        return m;
    }, [nodes]);
    const slidablesRef = useRef(slidables);
    slidablesRef.current = slidables;

    // 8.6: how far the document may scroll -- the rubber band's home range.
    // Top of the document at the top margin down to its bottom at the
    // bottom edge; a document shorter than the view just sits at the top.
    const scrollBounds = useRef({ min: 8, max: 8 });
    useEffect(() => {
        const gh = laid?.height ?? 0;
        scrollBounds.current = {
            min: Math.min(8, viewHeight - gh - 8),
            max: 8,
        };
    }, [laid, viewHeight]);

    // 8.6: dragging the background scrolls the document -- vertically only,
    // like everything global here -- and keeps its momentum when let go.
    //
    // Not React Flow's panOnDrag: that is a d3 listener on the pane, which
    // fires before React's handlers, so turning it on would drag the canvas
    // along with every slide of a box (the trap 8.4 records). This is our own
    // listener on the wrapper instead, taking only presses that began on the
    // background -- a native listener here fires before React's synthetic
    // ones, so it filters by target rather than trusting stopPropagation.
    const paneFling = useRef<(() => void) | null>(null);
    useEffect(() => {
        const el = flowRef.current;
        if (el === null) return;
        let dragging = false;
        let lastY = 0;
        let samples: Sample[] = [];
        const down = (event: PointerEvent) => {
            paneFling.current?.();
            paneFling.current = null;
            if (event.button !== 0) return;
            const target = event.target as Element;
            if (target.closest(
                ".react-flow__node, .react-flow__handle," +
                " .react-flow__minimap, .react-flow__edge, button") !== null) {
                return;
            }
            dragging = true;
            lastY = event.clientY;
            samples = [{ t: performance.now(), p: event.clientY }];
            el.setPointerCapture(event.pointerId);
        };
        const readY = () => getViewport().y;
        const writeY = (y: number) => {
            const v = getViewport();
            setViewport({ ...v, y });
        };
        const spring = () => {
            const b = scrollBounds.current;
            const target = Math.min(Math.max(readY(), b.min), b.max);
            paneFling.current = springTo(readY, target, writeY);
        };
        const move = (event: PointerEvent) => {
            if (!dragging) return;
            let dy = event.clientY - lastY;
            lastY = event.clientY;
            const now = performance.now();
            samples.push({ t: now, p: event.clientY });
            trimSamples(samples, now);
            const y = readY();
            const b = scrollBounds.current;
            // Past either end the drag pulls against the band.
            if ((y > b.max && dy > 0) || (y < b.min && dy < 0)) dy /= 3;
            writeY(y + dy);
        };
        const up = () => {
            if (!dragging) return;
            dragging = false;
            const b = scrollBounds.current;
            const y = readY();
            if (y < b.min || y > b.max) {
                spring();
                return;
            }
            const velocity = releaseVelocity(samples);
            if (Math.abs(velocity) > 0.05) {
                paneFling.current = fling(velocity, (d) => {
                    const ny = readY() + d;
                    writeY(ny);
                    const bounds = scrollBounds.current;
                    if (ny < bounds.min || ny > bounds.max) {
                        spring();
                        return false;
                    }
                });
            }
        };
        el.addEventListener("pointerdown", down);
        el.addEventListener("pointermove", move);
        el.addEventListener("pointerup", up);
        el.addEventListener("pointercancel", up);
        return () => {
            paneFling.current?.();
            el.removeEventListener("pointerdown", down);
            el.removeEventListener("pointermove", move);
            el.removeEventListener("pointerup", up);
            el.removeEventListener("pointercancel", up);
        };
    }, [getViewport, setViewport]);

    // 8.6's wheel, ahead of React Flow's own: Ctrl resizes the type, Shift
    // scrolls the slidable node under the pointer sideways (snapping one
    // clause at a time where the node snaps). Native and capturing -- React
    // Flow's pan is a d3 listener on a descendant, so only a capture on the
    // ancestor runs first; passive listeners cannot preventDefault, so not
    // that either.
    useEffect(() => {
        const el = flowRef.current;
        if (el === null) return;
        const onWheel = (event: WheelEvent) => {
            paneFling.current?.();
            paneFling.current = null;
            if (event.ctrlKey || event.metaKey) {
                event.preventDefault();
                event.stopPropagation();
                const step = event.deltaY > 0 ? -1 : 1;
                setFontPx((v) => Math.min(28, Math.max(7, v + step)));
                return;
            }
            if (!event.shiftKey) return;
            event.preventDefault();
            event.stopPropagation();
            const over = (event.target as Element)
                .closest?.("[data-id]")?.getAttribute("data-id");
            const info = over != null
                ? slidablesRef.current.get(over) : undefined;
            if (info === undefined) return;
            const delta = event.deltaY !== 0 ? event.deltaY : event.deltaX;
            if (info.stops !== undefined) {
                const at = nearestStop(info.stops, info.dx);
                const next = Math.min(info.stops.length - 1,
                    Math.max(0, at + (delta > 0 ? 1 : -1)));
                onSnapRef.current(info.key, info.stops[next]);
            } else if (info.axis === "x") {
                onSlideRef.current(info.key, delta > 0 ? -24 : 24, 0);
            } else {
                onSlideRef.current(info.key, 0, delta > 0 ? -24 : 24);
            }
        };
        el.addEventListener("wheel", onWheel,
            { capture: true, passive: false });
        return () => el.removeEventListener("wheel", onWheel,
            { capture: true });
    }, []);

    // The map is mounted one render after the pane it belongs to.
    //
    // React Flow's MiniMap builds its drag handling in an effect keyed on the
    // pane's panZoom, but installs it in a *second* effect keyed on size and
    // the pannable flags -- nothing that changes when the first one finally
    // runs. Mounted in the same pass as the pane, the map sees panZoom still
    // null, builds nothing, and its installer no-ops on an instance that is
    // not there yet. The instance arrives a render later and is never
    // installed, so the map does not answer the pointer until something
    // resizes it -- which is why leaving the tab and coming back woke it up.
    //
    // A render behind the pane, panZoom is already there and both effects run
    // on the map's own first pass. Keyed to flowKey so a remount of the pane
    // (see the ReactFlow key below) puts the map a render behind again.
    const flowKey = trail.join(",");
    const [readyKey, setReadyKey] = useState<string>();
    useEffect(() => {
        setReadyKey(flowKey);
        place.current = true;
    }, [flowKey]);
    const paneReady = readyKey === flowKey;

    const onConnect = useCallback((connection: Connection) => {
        // Edges render in an svg layer below the nodes unless told otherwise,
        // and a data line that runs behind the boxes it connects says nothing.
        // Nesting gives a node z of parent+1 (depth ~13 here) and selection
        // adds 1000, so 2000 clears everything.
        setEdges((current) =>
            addEdge({ ...connection, animated: true, zIndex: 2000 }, current));
    }, [setEdges]);

    return (
        <div id="app">
            <div id="bar">
                <button
                    type="button"
                    className={trail.length > 0 ? "active" : undefined}
                    title={trail.length === 0
                        ? "Not inside a definition"
                        : "Leave this definition"}
                    disabled={trail.length === 0}
                    onMouseDown={keepFocusOff}
                    onClick={() => setTrail((t) => t.slice(0, -1))}
                >▲</button>
                {/* Says what pressing it does, not what state the view is in
                    -- the boxes show that themselves. "All" is meant: it drops
                    every fold set on a single node, so one press puts the whole
                    view in one state again. */}
                <button
                    type="button"
                    className={folded > 0 ? "active" : undefined}
                    title={folded > 0
                        ? "Open every definition"
                        : "Fold every definition shut"}
                    onMouseDown={keepFocusOff}
                    onClick={() => {
                        setFoldByDefault(folded === 0);
                        setFolds({});
                        place.current = true;
                    }}
                >{folded > 0 ? "Unfold All" : "Fold All"}</button>
                <button
                    type="button"
                    title="Smaller text"
                    disabled={fontPx <= 7}
                    onMouseDown={keepFocusOff}
                    onClick={() => setFontPx((v) => Math.max(7, v - 1))}
                >A−</button>
                <button
                    type="button"
                    title={`Larger text (now ${fontPx}px)`}
                    disabled={fontPx >= 28}
                    onMouseDown={keepFocusOff}
                    onClick={() => setFontPx((v) => Math.min(28, v + 1))}
                >A+</button>
                <span id="status">{note}</span>
            </div>
            {view !== undefined && view.path.length > 0 && reply !== undefined && (
                <div id="trail">
                    <button type="button" className="crumb"
                        onMouseDown={keepFocusOff}
                        onClick={() => setTrail([])}>(file)</button>
                    {view.path.map((step, index) => (
                        <React.Fragment key={step.start}>
                            <span className="sep">›</span>
                            <button type="button" className="crumb"
                                onMouseDown={keepFocusOff}
                                onClick={() => setTrail(trail.slice(0, index + 1))}>
                                {titleOf(step, reply.source)}
                            </button>
                        </React.Fragment>
                    ))}
                </div>
            )}
            <div
                id="flow"
                ref={flowRef}
                className={snapAnim ? "snap-anim" : undefined}
                style={{ "--lhat-scale": String(scale) } as React.CSSProperties}
            >
                <ReactFlow
                    // Remounting is what refits the viewport, so it is done
                    // only when the view is a different thing to look at --
                    // moving into or out of a definition. Folding is not:
                    // shutting a definition leaves the rest of the picture
                    // where it was, and re-zooming to whatever is left throws
                    // the reader off a diagram they had not finished reading.
                    key={flowKey}
                    nodes={nodes}
                    edges={flow.exec.concat(edges)}
                    onEdgesChange={onEdgesChange}
                    onConnect={onConnect}
                    nodeTypes={nodeTypes}
                    // 8.6: a document, not a canvas. The zoom is locked at 1
                    // -- growing the picture is the type-size buttons' job,
                    // a re-layout rather than a transform -- and the only
                    // global movement is vertical. What overflows sideways is
                    // a branch's, and the branch handles it itself.
                    minZoom={1}
                    maxZoom={1}
                    zoomOnScroll={false}
                    zoomOnPinch={false}
                    zoomOnDoubleClick={false}
                    zoomActivationKeyCode={null}
                    // Dragging never moves the view: a drag on a box slides
                    // its contents (8.3) and nothing else.
                    panOnDrag={false}
                    panOnScroll
                    panOnScrollMode={PanOnScrollMode.Vertical}
                    // The wheel is React Flow's own pan, which no spring of
                    // ours can catch -- so it is clamped hard to the same
                    // range the rubber band comes home to. Programmatic
                    // setViewport bypasses this, which is what lets the
                    // band overshoot at all.
                    translateExtent={[
                        [-1e9, -8],
                        [1e9, Math.max(
                            (laid?.height ?? 0) + 8, viewHeight - 8)],
                    ]}
                >
                    <Background />
                    {paneReady && <MiniMap pannable />}
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
