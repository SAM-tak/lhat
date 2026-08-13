// L^ (lhat) -- the graph view of a .lh file (06).
//
// A CustomTextEditorProvider rather than a plain webview panel: the document
// stays a text document, so the text editor and the graph are two views of
// one thing and VSCode handles saving, undo and dirty state. 06 の 3 章 keeps
// the graph read-only for now, so nothing here writes to the document.
//
// The webview cannot reach lhatls (07 の L3), so this asks on its behalf and
// forwards the answer.

import * as vscode from "vscode";
import type { LanguageClient } from "vscode-languageclient/node";
import type { AstReply, FromWebview, ToWebview } from "./protocol";

export class LhatGraphEditorProvider implements vscode.CustomTextEditorProvider {
    public static readonly viewType = "lhat.graph";

    public constructor(
        private readonly context: vscode.ExtensionContext,
        private readonly client: () => LanguageClient | undefined,
    ) { }

    public async resolveCustomTextEditor(
        document: vscode.TextDocument,
        panel: vscode.WebviewPanel,
        _token: vscode.CancellationToken,
    ): Promise<void> {
        panel.webview.options = {
            enableScripts: true,
            localResourceRoots: [this.context.extensionUri],
        };
        panel.webview.html = this.html(panel.webview);

        const post = (message: ToWebview) => void panel.webview.postMessage(message);

        const send = async (): Promise<void> => {
            const client = this.client();
            if (client === undefined) {
                post({ type: "error", message: "the language server is not running" });
                return;
            }
            try {
                // 06 の 4.3: null until the unit is part of a checked root.
                const reply = await client.sendRequest<AstReply | null>("lhat/ast", {
                    textDocument: { uri: document.uri.toString() },
                });
                post(reply === null
                    ? { type: "pending" }
                    : { type: "tree", reply, uri: document.uri.toString() });
            } catch (error: unknown) {
                const reason = error instanceof Error ? error.message : String(error);
                post({ type: "error", message: `lhat/ast failed: ${reason}` });
            }
        };

        // V2, for now: ask again whenever this document changes. The server
        // re-checks on didChange, so a request that lands before it finishes
        // gets the previous tree or a null; the next change asks again.
        const changed = vscode.workspace.onDidChangeTextDocument((event) => {
            if (event.document.uri.toString() === document.uri.toString()) {
                void send();
            }
        });
        panel.onDidDispose(() => changed.dispose());

        panel.webview.onDidReceiveMessage((message: FromWebview) => {
            switch (message.type) {
                case "ready":
                case "refresh":
                    void send();
                    break;
                case "reveal":
                    void this.reveal(document, message.start, message.end);
                    break;
            }
        });
    }

    /** Put the text cursor on what was clicked in the graph. */
    private async reveal(
        document: vscode.TextDocument, start: number, end: number,
    ): Promise<void> {
        // 06 の 4.1: the offsets are UTF-16 code units, which is what
        // positionAt counts.
        const range = new vscode.Range(
            document.positionAt(start), document.positionAt(end));
        const editor = await vscode.window.showTextDocument(document, {
            viewColumn: vscode.ViewColumn.Beside,
            preserveFocus: true,
        });
        editor.selection = new vscode.Selection(range.start, range.end);
        editor.revealRange(range, vscode.TextEditorRevealType.InCenterIfOutsideViewport);
    }

    // 06 の 8.4: React Flow. One bundled script -- React, React Flow and
    // elkjs together, built by esbuild -- and its stylesheet. Styles need
    // 'unsafe-inline': React Flow positions its nodes by writing style
    // attributes, which is also how the layout's own sizes reach the boxes.
    private html(webview: vscode.Webview): string {
        const asset = (...parts: string[]) =>
            webview.asWebviewUri(vscode.Uri.joinPath(this.context.extensionUri, ...parts));

        const script = asset("media", "rf", "bundle.js");
        const bundleCss = asset("media", "rf", "bundle.css");
        const sharedCss = asset("media", "graph.css");
        const nonce = String(Math.random()).slice(2);

        return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none';
  style-src ${webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}';">
<link href="${sharedCss}" rel="stylesheet">
<link href="${bundleCss}" rel="stylesheet">
<title>L^ graph</title>
</head>
<body>
<div id="root"></div>
<script nonce="${nonce}" src="${script}"></script>
</body>
</html>`;
    }
}
