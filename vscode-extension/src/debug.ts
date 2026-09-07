// L^ (lhat) -- VSCode extension: running a program under the debugger.
//
// 09 の 7 章. The adapter is the runtime itself: `lhat --dap=PORT FILE` binds
// that port on loopback, waits for one debugger, and speaks DAP over it
// (dap/adapter.c). So the extension contributes no adapter of its own. It
// picks the port, starts the process, and hands VSCode a socket to it.
//
// Two things follow from the adapter being the program:
//
//   * the program's own stdout and stderr are the process's, not `output`
//     events -- 09 の D2 leaves that event unimplemented on purpose, since
//     the run is what a debugger drives rather than something it wraps -- so
//     this drains both pipes into the debug console, which is what makes a
//     print visible
//   * the socket goes up only once the program has been loaded and checked,
//     and a program with a type error never gets that far. So VSCode must
//     not be handed a port before there is something listening on it: the
//     runtime says when (one line on stderr), and this waits for the line or
//     for the process to die with its diagnostics.

import { ChildProcess, spawn } from "child_process";
import * as net from "net";
import * as path from "path";
import * as vscode from "vscode";

// What dap/adapter.c prints once the socket is up, and nothing else does.
// The newline is part of it: a chunk that split the number would otherwise
// match the digits it happened to carry, and the port would be wrong.
const LISTENING = /^lhat: dap listening on (\d+)\r?\n/m;

// How long to wait for that line. Loading and checking a program is what
// happens first, so this is generous -- what it is really guarding against is
// a runtime that never listens at all.
const LISTEN_TIMEOUT_MS = 60000;

function resolveRuntimeCommand(): string {
    const configured = vscode.workspace
        .getConfiguration("lhat")
        .get<string>("runtimePath");
    if (configured && configured.trim().length > 0) {
        return configured;
    }
    // No path configured: let the OS resolve it off PATH, the way
    // resolveServerCommand does for lhatls.
    return process.platform === "win32" ? "lhat.exe" : "lhat";
}

// A port nothing is listening on, found by listening on one and letting go.
// Another process could take it in between; SO_REUSEADDR is set on the
// runtime's side (port/socket.c) and the window is a few microseconds, which
// is the same bargain every adapter that starts a server makes.
function freePort(): Promise<number> {
    return new Promise((resolve, reject) => {
        const probe = net.createServer();
        probe.once("error", reject);
        probe.listen(0, "127.0.0.1", () => {
            const found = probe.address();
            const port =
                typeof found === "object" && found !== null ? found.port : 0;
            probe.close(() => {
                if (port > 0) {
                    resolve(port);
                } else {
                    reject(new Error("no free loopback port"));
                }
            });
        });
    });
}

// 09 の 7 章: the program's output belongs in the debug console. There is no
// per-session console in the stable API, only the active one -- so two
// sessions running at once share it. That is the whole of what is lost by
// not proxying the protocol to add `output` events, and it is not worth a
// second implementation of the wire format to get back.
function say(text: string): void {
    vscode.debug.activeDebugConsole.append(text);
}

// Starts the runtime and answers where to connect. Rejects with whatever the
// process said, which for the usual failure -- a type error -- is the
// diagnostics themselves.
function startRuntime(
    command: string,
    args: string[],
    options: { cwd?: string; env?: NodeJS.ProcessEnv },
): Promise<{ child: ChildProcess; port: number }> {
    return new Promise((resolve, reject) => {
        let child: ChildProcess;
        try {
            child = spawn(command, args, {
                cwd: options.cwd,
                env: options.env,
                windowsHide: true,
            });
        } catch (reason) {
            reject(reason);
            return;
        }

        let settled = false;
        let said = "";
        const finish = (act: () => void) => {
            if (!settled) {
                settled = true;
                clearTimeout(timer);
                act();
            }
        };

        const timer = setTimeout(() => {
            finish(() => {
                child.kill();
                reject(
                    new Error(
                        `${command} did not open a debug port within ` +
                            `${LISTEN_TIMEOUT_MS / 1000}s.${said ? "\n" + said : ""}`,
                    ),
                );
            });
        }, LISTEN_TIMEOUT_MS);

        child.stdout?.on("data", (chunk: Buffer) => say(chunk.toString()));

        // The announcement shares stderr with the runtime's own messages --
        // a --relaxed warning, a panic's traceback -- so the line is taken
        // out of the stream and everything else goes on through. Until it
        // arrives the rest is held rather than shown: if the program does not
        // check, what was held is the diagnostics, and they are the error
        // this rejects with.
        child.stderr?.on("data", (chunk: Buffer) => {
            if (settled) {
                say(chunk.toString());
                return;
            }
            said += chunk.toString();
            const found = LISTENING.exec(said);
            if (found === null) {
                return;
            }
            const rest =
                said.slice(0, found.index) +
                said.slice(found.index + found[0].length);
            said = "";
            finish(() => resolve({ child, port: Number(found[1]) }));
            if (rest.length > 0) {
                say(rest);
            }
        });

        child.once("error", (reason) => {
            finish(() =>
                reject(
                    new Error(
                        `could not start ${command}: ${reason.message}\n` +
                            `Set "lhat.runtimePath" to the path of lhat(.exe).`,
                    ),
                ),
            );
        });

        child.once("exit", (code) => {
            finish(() =>
                reject(
                    new Error(
                        said.trim().length > 0
                            ? said
                            : `${command} ended (code ${code ?? 0}) before ` +
                                  `opening a debug port.`,
                    ),
                ),
            );
        });
    });
}

// Fills in what a bare F5 leaves out, so a .lh file can be run without a
// launch.json at all.
export class LhatDebugConfigurationProvider
    implements vscode.DebugConfigurationProvider
{
    resolveDebugConfiguration(
        folder: vscode.WorkspaceFolder | undefined,
        configuration: vscode.DebugConfiguration,
    ): vscode.DebugConfiguration | undefined {
        if (!configuration.type && !configuration.request && !configuration.name) {
            // F5 with no launch.json: debug what is open, if it is ours.
            const open = vscode.window.activeTextEditor;
            if (open?.document.languageId !== "lhat") {
                return undefined;  // let VSCode offer its own choices
            }
            configuration.type = "lhat";
            configuration.request = "launch";
            configuration.name = path.basename(open.document.fileName);
            configuration.program = open.document.fileName;
        }
        if (!configuration.program) {
            void vscode.window.showErrorMessage(
                'The "lhat" debug configuration needs a "program".',
            );
            return undefined;
        }
        if (!configuration.cwd && folder !== undefined) {
            configuration.cwd = folder.uri.fsPath;
        }
        return configuration;
    }
}

export class LhatDebugAdapterFactory
    implements vscode.DebugAdapterDescriptorFactory, vscode.Disposable
{
    // Every runtime this factory started and that has not ended. VSCode ends
    // a session by closing the socket, which the runtime takes as a
    // disconnect -- these are for the case where it does not get that far.
    private readonly running = new Set<ChildProcess>();

    async createDebugAdapterDescriptor(
        session: vscode.DebugSession,
    ): Promise<vscode.DebugAdapterDescriptor> {
        const configuration = session.configuration;
        const port = await freePort();
        const command = resolveRuntimeCommand();

        // 01 の usage: the options come first, the file next, and everything
        // after it is the program's own '...' (02 の 13.7). --dap= implies
        // --run, so it is not written again.
        const args = [`--dap=${port}`];
        if (configuration.relaxed === true) {
            args.push("--relaxed");
        }
        args.push(configuration.program as string);
        for (const argument of (configuration.args as string[]) ?? []) {
            args.push(argument);
        }

        const started = await startRuntime(command, args, {
            cwd: configuration.cwd as string | undefined,
            env: { ...process.env, ...((configuration.env as object) ?? {}) },
        });
        this.running.add(started.child);
        started.child.once("exit", () => this.running.delete(started.child));
        return new vscode.DebugAdapterServer(started.port, "127.0.0.1");
    }

    dispose(): void {
        for (const child of this.running) {
            child.kill();
        }
        this.running.clear();
    }
}
