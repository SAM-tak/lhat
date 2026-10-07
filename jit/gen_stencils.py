#!/usr/bin/env python3
"""L^ (lhat) -- compile jit/stencils.c and write the stencils as C arrays.

Only someone changing the stencils runs this. What it writes is committed
(jit/stencils_<target>.h), so building the JIT needs neither Python nor any
particular Clang -- only the C compiler the rest of the build uses.

    python jit/gen_stencils.py --clang <path to clang>
    python jit/gen_stencils.py --clang <...> --check   # fail if out of date

The object is COFF, x86-64: the first target (jit.h). Each stencil is a
function in a section of its own (-ffunction-sections), so a section's bytes
are the function and its relocations are its holes. A relocation that names
anything but a hole -- a constant pool, a helper -- is refused: the bytes
are copied somewhere else, and nothing they could point at would follow.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = "x86_64-pc-windows-msvc"
OUTPUT = os.path.join(ROOT, "jit", "stencils_x86_64-windows.h")
PREFIX = "lhat_jit_s_"

# jit.h's LhatJitHoleKind, by the symbol each one is written against.
HOLES = {
    "_JIT_A": "LHAT_JIT_HOLE_A",
    "_JIT_B": "LHAT_JIT_HOLE_B",
    "_JIT_C": "LHAT_JIT_HOLE_C",
    "_JIT_K": "LHAT_JIT_HOLE_K",
    "_JIT_KTAG": "LHAT_JIT_HOLE_KTAG",
    "_JIT_PC": "LHAT_JIT_HOLE_PC",
    "_JIT_CONTINUE": "LHAT_JIT_HOLE_CONTINUE",
    "_JIT_TARGET": "LHAT_JIT_HOLE_TARGET",
    "_JIT_CALL": "LHAT_JIT_HOLE_CALL",
    "_JIT_RETURN": "LHAT_JIT_HOLE_RETURN",
    "_JIT_STEP": "LHAT_JIT_HOLE_STEP",
}

IMAGE_REL_AMD64_ADDR64 = 0x0001

# movabs $imm64, %rax ; jmp *%rax -- the tail call the large code model
# writes. The hole is the immediate, two bytes in.
TAIL = (b"\x48\xb8", b"\x48\xff\xe0")
TAIL_SIZE = 13


def compile_stencils(clang):
    handle, obj = tempfile.mkstemp(suffix=".o")
    os.close(handle)
    command = [
        clang, "--target=" + TARGET, "-std=c11", "-O3", "-c",
        os.path.join(ROOT, "jit", "stencils.c"), "-o", obj,
        "-I", os.path.join(ROOT, "include"), "-I", os.path.join(ROOT, "jit"),
        # Holes anywhere in the address space, one section per stencil, and
        # nothing the copied bytes could not carry with them.
        "-mcmodel=large", "-ffunction-sections",
        "-fno-asynchronous-unwind-tables", "-fno-stack-protector",
        "-fno-jump-tables", "-fno-builtin",
        "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
    ]
    subprocess.run(command, check=True)
    with open(obj, "rb") as f:
        data = f.read()
    os.remove(obj)
    return data


def read_coff(data):
    (machine, nsections, _, symtab, nsymbols, opt_size, _) = struct.unpack_from(
        "<HHIIIHH", data, 0)
    if machine != 0x8664:
        sys.exit("not an x86-64 COFF object")
    strtab = symtab + nsymbols * 18

    def string_at(offset):
        end = data.index(b"\0", strtab + offset)
        return data[strtab + offset:end].decode()

    def short_name(raw):
        if raw[:4] == b"\0\0\0\0":
            return string_at(struct.unpack_from("<I", raw, 4)[0])
        return raw.rstrip(b"\0").decode()

    sections = []
    base = 20 + opt_size
    for i in range(nsections):
        (raw_name, _, _, size, pointer, reloc_pointer, _, nrelocs, _,
         _) = struct.unpack_from("<8sIIIIIIHHI", data, base + i * 40)
        sections.append({
            "bytes": data[pointer:pointer + size],
            "relocs": [struct.unpack_from("<IIH", data, reloc_pointer + r * 10)
                       for r in range(nrelocs)],
        })

    symbols = {}
    names = []
    i = 0
    while i < nsymbols:
        (raw, value, section, _, _, naux) = struct.unpack_from(
            "<8sIhHBB", data, symtab + i * 18)
        name = short_name(raw)
        names.append(name)
        names.extend([None] * naux)
        if name.startswith(PREFIX) and section > 0:
            symbols[name[len(PREFIX):]] = (section - 1, value)
        i += 1 + naux
    return sections, symbols, names


def extract(sections, symbols, names):
    stencils = {}
    for stencil, (index, value) in sorted(symbols.items()):
        if value != 0:
            sys.exit(f"{stencil}: not at the start of its section")
        code = sections[index]["bytes"]
        holes = []
        for (offset, symbol, kind) in sections[index]["relocs"]:
            name = names[symbol]
            if name not in HOLES or kind != IMAGE_REL_AMD64_ADDR64:
                sys.exit(f"{stencil}: refers to {name} (relocation {kind}), "
                         "which is not a hole")
            addend = struct.unpack_from("<q", code, offset)[0]
            holes.append((offset, HOLES[name], addend))
        tail = len(code)
        if (len(code) >= TAIL_SIZE and
                code[-TAIL_SIZE:-TAIL_SIZE + 2] == TAIL[0] and
                code[-3:] == TAIL[1] and
                (len(code) - TAIL_SIZE + 2, "LHAT_JIT_HOLE_CONTINUE", 0)
                in holes):
            tail = len(code) - TAIL_SIZE
        stencils[stencil] = (code, tail, holes)
    return stencils


def render(stencils):
    out = [
        "// Generated by jit/gen_stencils.py from jit/stencils.c -- do not "
        "edit.",
        f"// Target: {TARGET}.",
        "",
        "#ifndef LHAT_JIT_STENCILS_H",
        "#define LHAT_JIT_STENCILS_H",
        "",
        '#include "jit.h"',
        "",
    ]
    for name, (code, tail, holes) in stencils.items():
        out.append(f"static const uint8_t lhat_jit_code_{name}[] = {{")
        for i in range(0, len(code), 12):
            out.append("    " + " ".join(f"0x{b:02x}," for b in code[i:i + 12]))
        out.append("};")
        if holes:
            out.append(f"static const LhatJitHole lhat_jit_holes_{name}[] = {{")
            for (offset, kind, addend) in holes:
                out.append(f"    {{{offset}, {kind}, {addend}}},")
            out.append("};")
        holes_name = f"lhat_jit_holes_{name}" if holes else "NULL"
        out.append(f"static const LhatJitStencil lhat_jit_stencil_{name} = {{")
        out.append(f"    lhat_jit_code_{name}, {len(code)}, {tail}, "
                   f"{holes_name}, {len(holes)}")
        out.append("};")
        out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--check", action="store_true",
                        help="compare with the committed header, write nothing")
    args = parser.parse_args()

    text = render(extract(*read_coff(compile_stencils(args.clang))))
    if args.check:
        with open(OUTPUT, encoding="utf-8") as f:
            if f.read() != text:
                sys.exit(f"{os.path.relpath(OUTPUT, ROOT)} is out of date: "
                         "run jit/gen_stencils.py")
        return
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
