// L^ (lhat) -- VSCode extension: a thin client that spawns lhatls (the
// language server binary; source lives under lsp/, built as target
// lhat_lsp) and speaks LSP to it over stdio. Diagnostics only for now
// (MVP) -- no hover, completion or go-to-definition; textDocumentSync is
// Full, matching lhatls's own capabilities response
// (lsp/handlers/initialize.c).

import * as vscode from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
} from "vscode-languageclient/node";

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

    context.subscriptions.push({
        dispose: () => {
            void client?.stop();
        },
    });
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
