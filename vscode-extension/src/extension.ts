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
    ErrorCodes,
    LanguageClient,
    LanguageClientOptions,
    ResponseError,
    ServerOptions,
    State,
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
        "L^ Language Server",
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

    // 07 の 4 章: the hover shows an elided type, which reads better in a
    // popup but is not what anyone can paste. The editor's own copy button
    // on a hover takes the whole popup -- the line, the type, the comments --
    // and neither that nor what it copies is ours to change. So the way to
    // get a signature worth keeping is to ask for one.
    //
    // Every way this can end says something. A command reached from a menu
    // gives no sign of having run, so one that returns quietly is
    // indistinguishable from one that was never pressed -- and the first
    // thing this hit in practice was an lhatls built before lhat/signature
    // existed, which answered -32601 into a silence.
    context.subscriptions.push(
        vscode.commands.registerCommand("lhat.copySignature", async () => {
            const editor = vscode.window.activeTextEditor;
            if (editor === undefined) {
                void vscode.window.showInformationMessage(
                    "Open a .lh file first.");
                return;
            }
            if (client === undefined || client.state !== State.Running) {
                void vscode.window.showWarningMessage(
                    "lhatls is not running, so there is nothing to ask. " +
                    "Run \"L^: Restart Language Server\".");
                return;
            }

            let reply: { signature: string } | null | undefined;
            try {
                reply = await client.sendRequest<{ signature: string } | null>(
                    "lhat/signature",
                    {
                        textDocument:
                            client.code2ProtocolConverter.asTextDocumentIdentifier(
                                editor.document),
                        position: client.code2ProtocolConverter.asPosition(
                            editor.selection.active),
                    },
                );
            } catch (error: unknown) {
                // An lhatls older than this command: it knows every other
                // method, so the client came up and nothing looks wrong until
                // this one is pressed.
                if (error instanceof ResponseError &&
                    error.code === ErrorCodes.MethodNotFound) {
                    void vscode.window.showErrorMessage(
                        "This lhatls is older than Copy Signature. Rebuild it " +
                        "(cmake --build --preset release) and run " +
                        "\"L^: Restart Language Server\".");
                    return;
                }
                const reason =
                    error instanceof Error ? error.message : String(error);
                void vscode.window.showErrorMessage(
                    `Could not read the signature: ${reason}`);
                return;
            }

            if (reply === null || reply === undefined) {
                // The server answers null both where nothing at the position
                // names a type and where the file is not one of the units it
                // checked (workspace.h), so this names both rather than
                // guessing which.
                void vscode.window.showInformationMessage(
                    "No type is named at the cursor -- or this file is not " +
                    "part of the checked workspace.");
                return;
            }
            await vscode.env.clipboard.writeText(reply.signature);
            // The signature itself stays out of the message. A long type
            // would be wrapped or cut in a notification, and a cut one ends
            // in the same "…" the hover uses -- which would read as "what
            // you copied was elided", the one thing this command exists to
            // avoid. The length says it arrived whole instead.
            void vscode.window.showInformationMessage(
                `Copied the signature to the clipboard ` +
                `(${reply.signature.length} characters).`);
        }),
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
