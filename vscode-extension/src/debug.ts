// L^ (lhat) -- VSCode extension: running a program under the debugger.
//
// 09 の 7 章. The adapter is the runtime itself: `lhat --dap=PORT FILE` binds
// that port on loopback, waits for one debugger, and speaks DAP over it
// (dap/adapter.c). The extension picks the port and starts the process.
//
// Two things follow from the adapter being the program:
//
//   * the program's own stdout and stderr are the process's, not `output`
//     events. The small inline adapter below relays the runtime's DAP socket
//     and makes those two pipes DAP output events. That is deliberately the
//     only protocol work here: VSCode consequently associates the output
//     with this session rather than whichever debug console is active
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

type OutputCategory = "stdout" | "stderr";

interface ProgramOutputChunk {
    category: OutputCategory;
    text: string;
}

// The runtime can print while startRuntime is still waiting for its listening
// line. Keep that output until there is an inline adapter to send it through;
// a DebugSession does not exist before then, so it cannot safely go to one.
class ProgramOutput {
    private sink: ((chunk: ProgramOutputChunk) => void) | undefined;
    private pending: ProgramOutputChunk[] = [];

    write(category: OutputCategory, text: string): void {
        const chunk = { category, text };
        if (this.sink === undefined) {
            this.pending.push(chunk);
        } else {
            this.sink(chunk);
        }
    }

    attach(sink: (chunk: ProgramOutputChunk) => void): void {
        if (this.sink !== undefined) {
            throw new Error("a runtime's output already has a DAP session");
        }
        this.sink = sink;
        for (const chunk of this.pending) {
            sink(chunk);
        }
        this.pending = [];
    }
}

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

// Starts the runtime and answers where to connect. Rejects with whatever the
// process said, which for the usual failure -- a type error -- is the
// diagnostics themselves.
function startRuntime(
    command: string,
    args: string[],
    options: { cwd?: string; env?: NodeJS.ProcessEnv },
): Promise<{ child: ChildProcess; port: number; output: ProgramOutput }> {
    return new Promise((resolve, reject) => {
        const output = new ProgramOutput();
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

        child.stdout?.on("data", (chunk: Buffer) =>
            output.write("stdout", chunk.toString()));

        // The announcement shares stderr with the runtime's own messages --
        // a --relaxed warning, a panic's traceback -- so the line is taken
        // out of the stream and everything else goes on through. Until it
        // arrives the rest is held rather than shown: if the program does not
        // check, what was held is the diagnostics, and they are the error
        // this rejects with.
        child.stderr?.on("data", (chunk: Buffer) => {
            if (settled) {
                output.write("stderr", chunk.toString());
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
            finish(() =>
                resolve({ child, port: Number(found[1]), output }));
            if (rest.length > 0) {
                output.write("stderr", rest);
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

// The protocol endpoint is still the runtime. This proxy only frames messages
// on its way to and from that endpoint, replaces its outbound sequence values
// with one sequence space that also includes injected output events, and never
// interprets a request or response. Inline adapters are the stable VSCode API
// that lets an extension add such session-owned events.
class LhatDapOutputProxy implements vscode.DebugAdapter {
    private readonly sent = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
    readonly onDidSendMessage = this.sent.event;

    private socket: net.Socket | undefined;
    private connected = false;
    private disposed = false;
    private received = Buffer.alloc(0);
    private readonly waitingForSocket: vscode.DebugProtocolMessage[] = [];
    private readonly waitingForInitialize: ProgramOutputChunk[] = [];
    private outputReady = false;
    private nextSequence = 1;

    constructor(port: number, output: ProgramOutput) {
        output.attach((chunk) => this.sendOutput(chunk));

        const socket = net.createConnection({ host: "127.0.0.1", port });
        this.socket = socket;
        socket.on("connect", () => {
            this.connected = true;
            for (const message of this.waitingForSocket.splice(0)) {
                this.writeMessage(message);
            }
        });
        socket.on("data", (chunk: Buffer) => this.readMessages(chunk));
        // A listening socket went away between startRuntime and connect. The
        // debug service learns that its inline adapter ended just as it does
        // for a DebugAdapterServer whose socket dies.
        socket.on("error", () => socket.destroy());
    }

    handleMessage(message: vscode.DebugProtocolMessage): void {
        if (this.disposed) {
            return;
        }
        if (!this.connected) {
            this.waitingForSocket.push(message);
            return;
        }
        this.writeMessage(message);
    }

    dispose(): void {
        if (this.disposed) {
            return;
        }
        this.disposed = true;
        this.socket?.destroy();
        this.sent.dispose();
    }

    private writeMessage(message: vscode.DebugProtocolMessage): void {
        if (this.socket === undefined || this.disposed) {
            return;
        }
        const body = JSON.stringify(message);
        const framed = Buffer.from(
            `Content-Length: ${Buffer.byteLength(body, "utf8")}\r\n\r\n${body}`,
            "utf8",
        );
        this.socket.write(framed);
    }

    private readMessages(chunk: Buffer): void {
        this.received = Buffer.concat([this.received, chunk]);
        for (;;) {
            const crlfEnd = this.received.indexOf("\r\n\r\n");
            const lfEnd = this.received.indexOf("\n\n");
            const headerEnd = crlfEnd >= 0 ? crlfEnd : lfEnd;
            if (headerEnd < 0) {
                return;
            }
            const separatorLength = crlfEnd >= 0 ? 4 : 2;
            const header = this.received
                .subarray(0, headerEnd).toString("ascii");
            const length = /^Content-Length:\s*(\d+)\s*$/im.exec(header);
            if (length === null) {
                this.socket?.destroy(new Error("DAP peer sent no Content-Length"));
                return;
            }
            const bodyLength = Number(length[1]);
            const bodyStart = headerEnd + separatorLength;
            if (!Number.isSafeInteger(bodyLength) || bodyLength < 0) {
                this.socket?.destroy(new Error("DAP peer sent an invalid Content-Length"));
                return;
            }
            if (this.received.length < bodyStart + bodyLength) {
                return;
            }
            const body = this.received
                .subarray(bodyStart, bodyStart + bodyLength).toString("utf8");
            this.received = this.received.subarray(bodyStart + bodyLength);
            let parsed: unknown;
            try {
                parsed = JSON.parse(body);
            } catch {
                this.socket?.destroy(new Error("DAP peer sent invalid JSON"));
                return;
            }
            if (typeof parsed !== "object" || parsed === null ||
                Array.isArray(parsed)) {
                this.socket?.destroy(new Error("DAP peer sent a non-object message"));
                return;
            }
            this.sendRuntimeMessage(parsed as Record<string, unknown>);
        }
    }

    private sendRuntimeMessage(message: Record<string, unknown>): void {
        this.sendMessage(message);
        if (message.type === "event" && message.event === "initialized") {
            this.outputReady = true;
            for (const chunk of this.waitingForInitialize.splice(0)) {
                this.sendOutput(chunk);
            }
        }
    }

    private sendOutput(chunk: ProgramOutputChunk): void {
        if (this.disposed) {
            return;
        }
        if (!this.outputReady) {
            this.waitingForInitialize.push(chunk);
            return;
        }
        this.sendMessage({
            type: "event",
            event: "output",
            body: {
                category: chunk.category,
                output: chunk.text,
            },
        });
    }

    private sendMessage(message: Record<string, unknown>): void {
        if (this.disposed) {
            return;
        }
        // DAP requires each side's seq to be one increasing sequence. The
        // runtime owns its sequence, while the proxy owns injected output;
        // assigning at this boundary keeps them in one unambiguous space.
        this.sent.fire({
            ...message,
            seq: this.nextSequence++,
        } as vscode.DebugProtocolMessage);
    }
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
        // This is not a second debugger: LhatDapOutputProxy relays every
        // request and runtime reply unchanged, adding only per-session
        // output events for the process pipes it already owns.
        return new vscode.DebugAdapterInlineImplementation(
            new LhatDapOutputProxy(started.port, started.output),
        );
    }

    dispose(): void {
        for (const child of this.running) {
            child.kill();
        }
        this.running.clear();
    }
}
