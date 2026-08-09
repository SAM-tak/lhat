// L^ (lhat) -- the graph webview.
//
// It cannot speak LSP itself, so the extension host asks lhatls for the tree
// and forwards it here (07 の L3). Everything from there down -- what is a
// container, which way it runs, what is folded, what a view is rooted at --
// is decided here, which is why 06 の 4.1 keeps all of it out of the reply.

// The browser resolves these itself -- no bundler -- so the specifiers carry
// the extension the emitted files actually have.
import type { AstNode, AstReply, FromWebview, ToWebview } from "../protocol.js";
import { isDrillTarget, nodeAt, titleOf, toElk } from "./map.js";
import { toSvg } from "./render.js";

declare function acquireVsCodeApi(): {
    postMessage(message: FromWebview): void;
    setState(state: unknown): void;
    getState(): unknown;
};

// elk.bundled.js defines a global ELK; it is loaded by a plain <script> so
// that the extension needs no bundler.
declare const ELK: new (options?: unknown) => {
    layout(graph: unknown): Promise<unknown>;
};

const vscode = acquireVsCodeApi();
const view = document.getElementById("view") as HTMLDivElement;
const status = document.getElementById("status") as HTMLDivElement;
const trailBar = document.getElementById("trail") as HTMLDivElement;

const elk = new ELK();
let collapse = true; // V15: definitions start folded

let latest: AstReply | undefined;
// 06 の 8.2: where the view has been drilled to, as the start position of each
// definition entered. Positions rather than nodes, so the trail survives the
// tree being replaced after an edit.
let trail: number[] = [];

function say(text: string): void {
    status.textContent = text;
}

/** The node the view is rooted at, and the definitions leading to it. */
function resolveTrail(reply: AstReply): { root: AstNode; path: AstNode[] } {
    const path: AstNode[] = [];
    let root = reply.root;
    for (const start of trail) {
        const found = nodeAt(reply.root, start);
        if (found === undefined) {
            // The edit removed what we were looking at. Stop where it still
            // makes sense rather than showing nothing.
            break;
        }
        path.push(found);
        root = found;
    }
    trail = path.map((node) => node.start);
    return { root, path };
}

function drawTrail(reply: AstReply, path: AstNode[]): void {
    trailBar.replaceChildren();
    if (path.length === 0) {
        trailBar.hidden = true;
        return;
    }
    trailBar.hidden = false;

    const step = (text: string, depth: number) => {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "crumb";
        button.textContent = text;
        button.addEventListener("click", () => {
            trail = trail.slice(0, depth);
            if (latest) void render(latest);
        });
        trailBar.append(button);
    };

    step("(file)", 0);
    path.forEach((node, i) => {
        const arrow = document.createElement("span");
        arrow.className = "sep";
        arrow.textContent = "›";
        trailBar.append(arrow);
        step(titleOf(node, reply.source), i + 1);
    });
}

async function render(reply: AstReply): Promise<void> {
    const started = performance.now();
    const { root, path } = resolveTrail(reply);
    drawTrail(reply, path);

    // Only when drilled in. toElk reads a root as "this view was opened at
    // that definition", and opens it -- so handing it the file's own root
    // would leave every definition unfolded whatever `collapse` says.
    const graph = toElk(reply, {
        collapse,
        root: path.length > 0 ? root : undefined,
    });
    const laid = await elk.layout(graph);
    view.innerHTML = toSvg(laid as never);
    const ms = Math.round(performance.now() - started);

    let nodes = 0;
    const count = (n: { children?: unknown[] }) => {
        nodes++;
        for (const c of (n.children ?? []) as { children?: unknown[] }[]) count(c);
    };
    count(laid as { children?: unknown[] });
    say(`${nodes} nodes, ${ms}ms` + (collapse ? ", definitions folded" : ""));

    for (const g of Array.from(view.querySelectorAll<SVGGElement>("g.n"))) {
        g.addEventListener("click", (event) => {
            event.stopPropagation();
            const start = Number(g.dataset.start);
            if (!Number.isFinite(start)) return;

            // 8.2: a folded definition is a way in, not a thing to select.
            // Everything else puts the text cursor on what it was made from.
            const node = nodeAt(reply.root, start);
            if (g.classList.contains("collapsed") && node !== undefined &&
                (isDrillTarget(node) || node.fields !== undefined)) {
                trail = [...trail, start];
                void render(reply);
                return;
            }
            const end = Number(g.dataset.end);
            if (Number.isFinite(end)) {
                vscode.postMessage({ type: "reveal", start, end });
            }
        });
    }
}

window.addEventListener("message", (event: MessageEvent<ToWebview>) => {
    const message = event.data;
    switch (message.type) {
        case "tree":
            latest = message.reply;
            void render(message.reply);
            break;
        case "pending":
            // 06 の 4.3: not checked yet. Diagnostics will follow, and the
            // host asks again when the document changes.
            say("waiting for the language server…");
            break;
        case "error":
            say(message.message);
            break;
    }
});

document.getElementById("fold")?.addEventListener("click", () => {
    collapse = !collapse;
    if (latest) void render(latest);
});

document.getElementById("up")?.addEventListener("click", () => {
    if (trail.length === 0) return;
    trail = trail.slice(0, -1);
    if (latest) void render(latest);
});

vscode.postMessage({ type: "ready" });
