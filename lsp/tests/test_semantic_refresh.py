"""End-to-end regression: python lsp/tests/test_semantic_refresh.py <lhatls>.

Request tokens inside the worker's debounce window, then verify that its
refresh repairs their coordinates without another edit (even with no errors).
"""

import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


class Client:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self.messages = queue.Queue()
        self.serial = 0
        self.refresh_ids = set()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        stream = self.process.stdout
        while True:
            headers = {}
            while True:
                line = stream.readline()
                if not line:
                    return
                if line == b"\r\n":
                    break
                name, value = line.decode().split(":", 1)
                headers[name.lower()] = value.strip()
            self.messages.put(json.loads(stream.read(int(headers["content-length"]))))

    def send(self, **message):
        body = json.dumps(dict(jsonrpc="2.0", **message)).encode()
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.process.stdin.flush()

    def wait(self, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            message = self.messages.get(timeout=max(0, deadline - time.monotonic()))
            if message.get("method") == "workspace/semanticTokens/refresh":
                assert "id" in message, "refresh must be a request, not a notification"
                assert message["id"] not in self.refresh_ids, "request ids must be unique"
                self.refresh_ids.add(message["id"])
                self.send(id=message["id"], result=None)
            if predicate(message):
                return message

    def request(self, method, params):
        self.serial += 1
        self.send(id=self.serial, method=method, params=params)
        response = self.wait(lambda m: m.get("id") == self.serial and "method" not in m)
        assert "error" not in response, response
        return response["result"]

    def refresh(self):
        return self.wait(lambda m: m.get("method") == "workspace/semanticTokens/refresh")

    def close(self):
        try:
            self.request("shutdown", None)
            self.send(method="exit")
            assert self.process.wait(timeout=5) == 0
        finally:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait()
            self.reader.join(timeout=5)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                stream.close()


def check(executable, refresh_support):
    with tempfile.TemporaryDirectory(prefix="lhat-semantic-refresh-") as directory:
        root = Path(directory)
        file = root / "main.lh"
        original = "let^value = 1\nreturn^value\n"
        file.write_text(original, encoding="utf-8")
        client = Client(executable)
        try:
            capabilities = {} if refresh_support is None else {
                "workspace": {"semanticTokens": {"refreshSupport": refresh_support}}}
            client.request("initialize", {"rootUri": root.as_uri(), "capabilities": capabilities})
            client.send(method="initialized", params={})
            if refresh_support:
                client.refresh()  # initial workspace analysis also invalidates early answers
            else:
                try:
                    client.wait(lambda m: m.get("method") == "workspace/semanticTokens/refresh", 0.8)
                except queue.Empty:
                    pass
                else:
                    raise AssertionError("refresh sent without client support")
                return

            document = {"uri": file.as_uri()}
            client.send(method="textDocument/didOpen", params={"textDocument": dict(
                document, languageId="lhat", version=1, text=original)})
            client.refresh()
            params = {"textDocument": document}
            baseline = client.request("textDocument/semanticTokens/full", params)["data"]
            assert baseline, "fixture must produce semantic tokens"

            # Whole-document replacement, undo, then newline insertion/removal.
            # Disk remains unchanged, exercising unsaved document synchronization.
            for version, prefix in enumerate(("\n\n    ", "", "\n", ""), start=2):
                client.send(method="textDocument/didChange", params={
                    "textDocument": dict(document, version=version),
                    "contentChanges": [{"text": prefix + original}]})
                client.request("textDocument/semanticTokens/full", params)
                client.refresh()
                actual = client.request("textDocument/semanticTokens/full", params)["data"]
                expected = baseline.copy()
                expected[0] += prefix.count("\n")
                expected[1] += len(prefix.rsplit("\n", 1)[-1])
                assert actual == expected, (prefix, expected, actual)
        finally:
            client.close()


if __name__ == "__main__":
    executable = str(Path(sys.argv[1]).resolve())
    for support in (True, False, None):
        check(executable, support)
        print(f"PASS semantic refresh (refreshSupport={support})")
