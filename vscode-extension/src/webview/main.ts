// L^ (lhat) -- the graph webview.
//
// It cannot speak LSP itself, so the extension host asks lhatls for the tree
// and forwards it here (07 の L3). Everything from there down -- what is a
// container, which way it runs, what is folded -- is decided here, which is
// why 06 の 4.1 keeps all of it out of the reply.

// The browser resolves these itself -- no bundler -- so the specifiers carry
// the extension the emitted files actually have.
import type { AstReply, FromWebview, ToWebview } from "../protocol.js";
import { toElk } from "./map.js";
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

const elk = new ELK();
let collapse = true; // V15: definitions start folded

let latest: AstReply | undefined;

function say(text: string): void {
    status.textContent = text;
}

async function render(reply: AstReply): Promise<void> {
    const started = performance.now();
    const graph = toElk(reply, { collapse });
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

    // Clicking a box puts the text cursor on what it was made from.
    for (const g of Array.from(view.querySelectorAll<SVGGElement>("g.n"))) {
        g.addEventListener("click", (event) => {
            event.stopPropagation();
            const start = Number(g.dataset.start);
            const end = Number(g.dataset.end);
            if (Number.isFinite(start) && Number.isFinite(end)) {
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

vscode.postMessage({ type: "ready" });
