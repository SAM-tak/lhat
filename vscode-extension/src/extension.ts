// L^ (lhat) -- VSCode extension: a thin client that spawns lhatls (the
// language server binary; source lives under lsp/, built as target
// lhat_lsp) and speaks LSP to it over stdio. Diagnostics and semantic
// tokens for now -- no hover, completion or go-to-definition; textDocumentSync
// is Full, matching lhatls's own capabilities response
// (lsp/handlers/initialize.c).
//
// It also hosts the graph view of 06 (graphEditor.ts), which reaches lhatls
// through this same client.

import * as vscode from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
} from "vscode-languageclient/node";
import { LhatGraphEditorProvider } from "./graphEditor";

let client: LanguageClient | undefined;

function resolveServerCommand(): string {
    const configured = vscode.workspace
        .getConfiguration("lhat")
        .get<string>("serverPath");
    if (configured && configured.trim().length > 0) {
        return configured;
    }
    // No path configured: let the OS resolve it off PATH, as clangd's
    // extension does for clangd itself.
    return process.platform === "win32" ? "lhatls.exe" : "lhatls";
}

export function activate(context: vscode.ExtensionContext): void {
    const command = resolveServerCommand();

    const serverOptions: ServerOptions = {
        command,
        args: [],
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "lhat" }],
    };

    client = new LanguageClient(
        "lhatLanguageServer",
        "L^ (lhat) Language Server",
        serverOptions,
        clientOptions
    );

    client.start().catch((error: unknown) => {
        const reason = error instanceof Error ? error.message : String(error);
        void vscode.window.showErrorMessage(
            `Could not launch lhatls (${command}): ${reason}\n` +
            `Set "lhat.serverPath" to the path of lhatls(.exe).`
        );
    });

    // 06: the graph view. Registered whether or not the server came up -- it
    // says so itself rather than being missing from the editor list.
    context.subscriptions.push(
        vscode.window.registerCustomEditorProvider(
            LhatGraphEditorProvider.viewType,
            new LhatGraphEditorProvider(context, () => client),
            { webviewOptions: { retainContextWhenHidden: true } },
        ),
    );

    // The custom editor is registered with priority "option", so opening a
    // .lh file still gives the text editor. This is how the graph is asked
    // for, beside the text.
    context.subscriptions.push(
        vscode.commands.registerCommand("lhat.openGraph", () => {
            const uri = vscode.window.activeTextEditor?.document.uri;
            if (uri === undefined) {
                void vscode.window.showInformationMessage(
                    "Open a .lh file first.");
                return;
            }
            // Another tab in the same group by default; a split only when
            // asked for. Splitting halves the width the graph has, and 06 の
            // 8.1 measured that width to be what runs out first.
            const beside = vscode.workspace
                .getConfiguration("lhat")
                .get<boolean>("graph.openBeside", false);
            void vscode.commands.executeCommand(
                "vscode.openWith", uri, LhatGraphEditorProvider.viewType,
                beside ? vscode.ViewColumn.Beside : vscode.ViewColumn.Active);
        }),
    );

    context.subscriptions.push({
        dispose: () => {
            void client?.stop();
        },
    });
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
