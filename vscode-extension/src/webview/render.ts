// L^ (lhat) -- draws a laid-out graph as SVG.
//
// 06 の 8.4 leaves the drawing layer open (V14), with React Flow as the
// candidate. This is deliberately not that: it is the smallest thing that
// puts the picture on screen, so the mapping can be looked at inside VSCode
// before anything is committed to. Panning, folding and partial scrolling
// (8.3) come with the real drawing layer.

import type { ElkNode } from "./map.js";

const esc = (s: string) =>
    s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");

// Depth decides the fill, so the nesting the two axes produce is visible at a
// glance rather than having to be traced edge by edge. Written against the
// editor's own theme variables so the picture follows light and dark.
const DEPTH_SHADES = 6;

function draw(node: ElkNode, depth: number, ox: number, oy: number, out: string[]): void {
    const x = ox + (node.x ?? 0);
    const y = oy + (node.y ?? 0);
    const w = node.width ?? 0;
    const h = node.height ?? 0;
    const isContainer = (node.children ?? []).length > 0;
    const label = node.labels?.[0]?.text ?? "";

    if (depth > 0) {
        const shade = Math.min(depth, DEPTH_SHADES);
        const cls = node.lhat?.collapsed
            ? "n collapsed"
            : isContainer
                ? `n box d${shade}`
                : "n leaf";
        // The span goes on the element so a click can reveal it in the text.
        const span = node.lhat
            ? ` data-start="${node.lhat.start}" data-end="${node.lhat.end}"`
            : "";
        out.push(`<g class="${cls}"${span}>` +
            `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="4"/>`);
        if (label) {
            // A container labels its header strip; a leaf labels its middle.
            const ty = isContainer ? y + 16 : y + h / 2 + 4;
            out.push(`<text x="${x + 8}" y="${ty}">${esc(label)}</text>`);
        }
        out.push(`</g>`);
    }

    for (const e of node.edges ?? []) {
        // 6.3: the order-pinning chain is a device for ELK, not something the
        // reader should see.
        if (e.pinned) continue;
        for (const s of e.sections ?? []) {
            const pts = [s.startPoint, ...(s.bendPoints ?? []), s.endPoint]
                .map((p) => `${x + p.x},${y + p.y}`).join(" ");
            out.push(`<polyline class="edge" points="${pts}"/>`);
        }
    }

    for (const c of node.children ?? []) draw(c, depth + 1, x, y, out);
}

export function toSvg(laid: ElkNode): string {
    const out: string[] = [];
    draw(laid, 0, 0, 0, out);
    const w = Math.ceil(laid.width ?? 0) + 2;
    const h = Math.ceil(laid.height ?? 0) + 2;
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" ` +
        `viewBox="0 0 ${w} ${h}">${out.join("")}</svg>`;
}
