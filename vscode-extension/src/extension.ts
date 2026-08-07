// L^ (lhat) -- VSCode extension: a thin client that spawns lhat-lsp and
// speaks LSP to it over stdio. Diagnostics only for now (MVP) -- no hover,
// completion or go-to-definition; textDocumentSync is Full, matching
// lhat-lsp's own capabilities response (lsp/handlers/initialize.c).

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
    return process.platform === "win32" ? "lhat-lsp.exe" : "lhat-lsp";
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
            `lhat-lsp を起動できません（${command}）: ${reason}\n` +
            `設定 "lhat.serverPath" で lhat-lsp(.exe) へのパスを指定してください。`
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
