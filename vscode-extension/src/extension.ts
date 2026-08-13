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
    CloseAction,
    ErrorAction,
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

function autoRestartEnabled(): boolean {
    // Whether this is a Marketplace/vsix install versus a self-built one
    // sideloaded over it is not something the extension API exposes --
    // ExtensionMode only tells the Extension Development Host (F5) apart
    // from everything else, and ships as Production either way. So this is
    // a setting, defaulting to the safe behaviour for a normal install.
    return vscode.workspace
        .getConfiguration("lhat")
        .get<boolean>("serverAutoRestart", true);
}

function startClient(command: string): LanguageClient {
    const serverOptions: ServerOptions = {
        command,
        args: [],
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: "file", language: "lhat" }],
        // The default ErrorHandler restarts lhatls on its own once its
        // process closes -- the right behaviour recovering from a real
        // crash, which is why it is the default. Turned off by
        // "lhat.serverAutoRestart": false for a self-built lhatls.exe
        // being iterated on: a rebuild kills the process to free the file
        // for the linker, and left enabled the client would respawn it
        // before the linker got a chance, LNK1104 following. "L^: Restart
        // Language Server" (below) reconnects once the new build lands.
        errorHandler: autoRestartEnabled()
            ? undefined
            : {
                  error: () => ({ action: ErrorAction.Continue }),
                  closed: () => ({ action: CloseAction.DoNotRestart }),
              },
    };

    const newClient = new LanguageClient(
        "lhatLanguageServer",
        "L^ (lhat) Language Server",
        serverOptions,
        clientOptions
    );

    newClient.start().catch((error: unknown) => {
        const reason = error instanceof Error ? error.message : String(error);
        void vscode.window.showErrorMessage(
            `Could not launch lhatls (${command}): ${reason}\n` +
            `Set "lhat.serverPath" to the path of lhatls(.exe).`
        );
    });

    return newClient;
}

export function activate(context: vscode.ExtensionContext): void {
    client = startClient(resolveServerCommand());

    context.subscriptions.push(
        vscode.commands.registerCommand("lhat.restartServer", async () => {
            await client?.stop();
            client = startClient(resolveServerCommand());
        }),
    );

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
