"""Explicitly fetch pinned Luau and official Lua; never runs during CMake builds."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import time
import urllib.request

from dependencies import lua_source_hash


def prepare_lua(dest, lock):
    if dest.exists():
        if lua_source_hash(dest) != lock["source_sha256"]:
            raise ValueError("Lua source differs from the pinned release; use a clean destination")
        print(dest)
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Verify the official archive before extracting anything. Reject paths and
    # links outside its single release directory, including on older Python.
    with tempfile.TemporaryDirectory(dir=dest.parent) as temporary:
        archive = Path(temporary) / "lua.tar.gz"
        urllib.request.urlretrieve(lock["url"], archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != lock["sha256"]:
            raise ValueError("Lua archive SHA-256 mismatch")
        prefix = "lua-" + lock["version"]
        with tarfile.open(archive) as tar:
            for member in tar.getmembers():
                parts = member.name.split("/")
                if parts[0] != prefix or ".." in parts or "\\" in member.name or not (member.isfile() or member.isdir()):
                    raise ValueError("unexpected Lua archive entry")
            tar.extractall(temporary)
        extracted = Path(temporary) / prefix
        if lua_source_hash(extracted) != lock["source_sha256"]:
            raise ValueError("Lua source SHA-256 mismatch")
        # Only publish the verified directory; never overwrite an existing one.
        # Windows indexers may briefly hold newly extracted files without
        # delete sharing. Keep the verified tree intact and retry the rename.
        for attempt in range(10):
            try:
                extracted.rename(dest)
                break
            except PermissionError:
                if attempt == 9:
                    raise
                time.sleep(0.05 * (attempt + 1))
    print(dest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=Path(__file__).resolve().parents[2] / "build/compare-deps/luau")
    parser.add_argument("--lua-dest", type=Path, help="default: build/compare-deps/lua-<pinned version>")
    args = parser.parse_args()
    lock = json.loads(Path(__file__).with_name("luau.lock.json").read_text())
    dest = args.dest.resolve()
    if not dest.exists():
        subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout", lock["repository"], str(dest)], check=True)
    if subprocess.check_output(["git", "-C", str(dest), "status", "--porcelain"]).strip():
        # A newly cloned --no-checkout worktree has all paths marked deleted.
        # Only allow that state when HEAD has never been checked out.
        if (dest / "CMakeLists.txt").exists() or (dest / ".git/index").exists():
            parser.error("Luau worktree has local changes; use a clean destination")
    found = subprocess.run(["git", "-C", str(dest), "cat-file", "-e", lock["commit"]], capture_output=True)
    if found.returncode:
        subprocess.run(["git", "-C", str(dest), "fetch", "origin", lock["commit"]], check=True)
    subprocess.run(["git", "-C", str(dest), "checkout", "--detach", lock["commit"]], check=True)
    print(dest)
    lua_lock = json.loads(Path(__file__).with_name("lua.lock.json").read_text())
    lua_dest = args.lua_dest or Path(__file__).resolve().parents[2] / "build/compare-deps" / ("lua-" + lua_lock["version"])
    prepare_lua(lua_dest.resolve(), lua_lock)


if __name__ == "__main__":
    main()
