"""Validate the C sources and headers of the pinned official Lua release."""
import hashlib
from pathlib import Path


def lua_source_hash(root):
    root = Path(root)
    files = sorted(path for path in (root / "src").iterdir() if path.suffix in (".c", ".h"))
    entries = "".join(path.relative_to(root).as_posix() + ":" + hashlib.sha256(path.read_bytes()).hexdigest() + "\n"
                      for path in files)
    return hashlib.sha256(entries.encode("utf-8")).hexdigest()
