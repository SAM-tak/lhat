#!/usr/bin/env python3
"""L^ (lhat) -- compile jit/stencils.c and write the stencils as C arrays.

Only someone changing the stencils runs this. What it writes is committed
(jit/stencils_<target>.h), so building the JIT needs neither Python nor any
particular Clang -- only the C compiler the rest of the build uses.

    python jit/gen_stencils.py --clang <path to clang>
    python jit/gen_stencils.py --check   # fail if a source changed since

The header records a hash of the sources it was made from (SOURCES below).
--check compares that and nothing else: the bytes themselves depend on
which Clang made them, so regenerating to compare would fail between two
machines whose stencils are equally right. What it catches is an edit to
a source that was never followed by a run of this.

The stencils run on x86-64 Windows (jit.h) but are compiled for an ELF
target, without position independence and under the small code model:
that is what gives a hole a 32-bit field in the instruction using it --
a displacement a load carries, an immediate, a rel32 jump -- where COFF
would reach every symbol through a RIP-relative address. The functions
say the Windows convention outright (LHAT_JIT_ABI). Nothing they read
differs in layout between the two targets, and the header carries the
proof: the offsets the ELF compile saw, asserted where the runtime is
compiled for Windows.

Each stencil is a function in a section of its own (-ffunction-sections),
so a section's bytes are the function and its relocations are its holes.
A relocation that names anything but a hole -- a constant pool, a helper
called directly -- is refused: the bytes are copied somewhere else, and
nothing they could point at would follow.
"""

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = "x86_64-unknown-linux-gnu"
OUTPUT = os.path.join(ROOT, "jit", "stencils_x86_64-windows.h")
PREFIX = "lhat_jit_s_"

# Everything the stencils' bytes are made from: their source and the layouts
# it reads (jit.h's contract).
SOURCES = ["jit/stencils.c", "jit/jit.h", "include/lhat/value.h",
           "include/lhat/object.h", "include/lhat/config.h"]
HASH_LINE = re.compile(r"^// Sources: sha256:([0-9a-f]{64})$", re.M)

# What the stencils read of the layouts they share with the runtime. The ELF
# compile measures them, and the header asserts the same numbers where the
# runtime is compiled.
LAYOUT = [
    "sizeof(LhatValueUnion)",
    "offsetof(LhatJitContext, steps_left)",
    "offsetof(LhatJitContext, traps)",
    "offsetof(LhatJitContext, values)",
    "offsetof(LhatJitContext, tags)",
    "offsetof(LhatJitContext, closure)",
    "offsetof(LhatJitContext, leave_pc)",
    "offsetof(LhatJitContext, environment)",
    "offsetof(LhatObject, kind)",
    "sizeof(((LhatObject *)0)->kind)",
    "offsetof(LhatClosure, upvalues)",
    "offsetof(LhatUpvalue, location)",
    "offsetof(LhatValueRef, value)",
    "offsetof(LhatValueRef, tag)",
    "offsetof(LhatTable, array)",
    "offsetof(LhatTable, array_count)",
    "offsetof(LhatSlots, values)",
    "offsetof(LhatSlots, tags)",
]

# jit.h's LhatJitHoleKind, by the symbol each one is written against.
HOLES = {
    "_JIT_A": "LHAT_JIT_HOLE_A",
    "_JIT_B": "LHAT_JIT_HOLE_B",
    "_JIT_C": "LHAT_JIT_HOLE_C",
    "_JIT_A8": "LHAT_JIT_HOLE_A8",
    "_JIT_B8": "LHAT_JIT_HOLE_B8",
    "_JIT_C8": "LHAT_JIT_HOLE_C8",
    "_JIT_K_LO": "LHAT_JIT_HOLE_K_LO",
    "_JIT_K_HI": "LHAT_JIT_HOLE_K_HI",
    "_JIT_KTAG": "LHAT_JIT_HOLE_KTAG",
    "_JIT_PC": "LHAT_JIT_HOLE_PC",
    "_JIT_CONTINUE": "LHAT_JIT_HOLE_CONTINUE",
    "_JIT_TARGET": "LHAT_JIT_HOLE_TARGET",
    "_JIT_CALL": "LHAT_JIT_HOLE_CALL",
    "_JIT_RETURN": "LHAT_JIT_HOLE_RETURN",
    "_JIT_STEP": "LHAT_JIT_HOLE_STEP",
}

# ELF x86-64 relocation types, by the form jit.h writes them in.
FORMS = {
    1: "LHAT_JIT_FORM_ABS64",    # R_X86_64_64
    10: "LHAT_JIT_FORM_ABS32",   # R_X86_64_32
    11: "LHAT_JIT_FORM_ABS32S",  # R_X86_64_32S
    2: "LHAT_JIT_FORM_REL32",    # R_X86_64_PC32
    4: "LHAT_JIT_FORM_REL32",    # R_X86_64_PLT32
}

# jmp rel32 -- the tail call to the next instruction's code. The field is
# the four bytes after the opcode.
TAIL_OPCODE = 0xE9
TAIL_SIZE = 5

FLAGS = [
    "--target=" + TARGET, "-std=gnu11", "-O3", "-c",
    "-I", os.path.join(ROOT, "include"), "-I", os.path.join(ROOT, "jit"),
    # 32-bit fields for every hole but the helpers, one section per stencil,
    # and nothing the copied bytes could not carry with them.
    "-fno-pic", "-mcmodel=small", "-ffunction-sections", "-fdata-sections",
    "-fno-asynchronous-unwind-tables", "-fno-stack-protector",
    "-fno-jump-tables", "-fno-builtin",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
]


def sources_hash():
    digest = hashlib.sha256()
    for path in SOURCES:
        with open(os.path.join(ROOT, path), "rb") as f:
            # Line endings are a checkout's, not the source's.
            digest.update(f.read().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def compile_c(clang, source_path):
    handle, obj = tempfile.mkstemp(suffix=".o")
    os.close(handle)
    subprocess.run([clang, *FLAGS, source_path, "-o", obj], check=True)
    with open(obj, "rb") as f:
        data = f.read()
    os.remove(obj)
    return data


def read_elf(data):
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        sys.exit("not a little-endian ELF64 object")
    (machine,) = struct.unpack_from("<H", data, 18)
    if machine != 62:
        sys.exit("not an x86-64 object")
    (shoff,) = struct.unpack_from("<Q", data, 40)
    (shentsize, shnum, shstrndx) = struct.unpack_from("<HHH", data, 58)

    sections = []
    for i in range(shnum):
        (name, kind, _, _, offset, size, link, info, _,
         entsize) = struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        sections.append({"name": name, "type": kind, "offset": offset,
                         "size": size, "link": link, "info": info,
                         "entsize": entsize})

    def string_at(table, offset):
        start = sections[table]["offset"] + offset
        return data[start:data.index(b"\0", start)].decode()

    for section in sections:
        section["name"] = string_at(shstrndx, section["name"])
        section["bytes"] = (b"" if section["type"] == 8  # SHT_NOBITS
                            else data[section["offset"]:
                                      section["offset"] + section["size"]])
        section["relocs"] = []

    symtab = next(i for i, s in enumerate(sections) if s["type"] == 2)
    symbols = []
    table = sections[symtab]
    for i in range(table["size"] // 24):
        (name, _, _, index, value, _) = struct.unpack_from(
            "<IBBHQQ", data, table["offset"] + i * 24)
        symbols.append((string_at(table["link"], name), index, value))

    for section in sections:
        if section["type"] != 4:  # SHT_RELA
            continue
        target = sections[section["info"]]
        for i in range(section["size"] // 24):
            (offset, info, addend) = struct.unpack_from(
                "<QQq", data, section["offset"] + i * 24)
            target["relocs"].append(
                (offset, symbols[info >> 32][0], info & 0xFFFFFFFF, addend))
    return sections, symbols


def extract(sections, symbols):
    stencils = {}
    for (name, index, value) in sorted(symbols):
        if not name.startswith(PREFIX) or index == 0 or index >= 0xFF00:
            continue
        stencil = name[len(PREFIX):]
        if value != 0:
            sys.exit(f"{stencil}: not at the start of its section")
        code = sections[index]["bytes"]
        holes = []
        for (offset, symbol, kind, addend) in sections[index]["relocs"]:
            if symbol not in HOLES or kind not in FORMS:
                sys.exit(f"{stencil}: refers to {symbol or '(a section)'} "
                         f"(relocation {kind}), which is not a hole")
            holes.append((offset, HOLES[symbol], FORMS[kind], addend))
        tail = len(code)
        if (len(code) >= TAIL_SIZE and
                code[-TAIL_SIZE] == TAIL_OPCODE and
                any(h[0] == len(code) - 4 and
                    h[1] == "LHAT_JIT_HOLE_CONTINUE" and
                    h[2] == "LHAT_JIT_FORM_REL32" for h in holes)):
            tail = len(code) - TAIL_SIZE
        stencils[stencil] = (code, tail, holes)
    return stencils


def measure_layout(clang):
    """The LAYOUT expressions as the ELF compile sees them."""
    handle, probe = tempfile.mkstemp(suffix=".c")
    with os.fdopen(handle, "w") as f:
        f.write('#include <stddef.h>\n#include "jit.h"\n'
                '#include "lhat/object.h"\n'
                "const unsigned long long lhat_jit_layout[] = {\n")
        f.writelines(f"    {expression},\n" for expression in LAYOUT)
        f.write("};\n")
    try:
        sections, symbols = read_elf(compile_c(clang, probe))
    finally:
        os.remove(probe)
    (_, index, value) = next(s for s in symbols if s[0] == "lhat_jit_layout")
    data = sections[index]["bytes"]
    return [struct.unpack_from("<Q", data, value + i * 8)[0]
            for i in range(len(LAYOUT))]


def render(stencils, layout):
    out = [
        "// Generated by jit/gen_stencils.py from jit/stencils.c -- do not "
        "edit.",
        f"// Compiled for {TARGET}, run on x86-64 Windows.",
        f"// Sources: sha256:{sources_hash()}",
        "",
        "#ifndef LHAT_JIT_STENCILS_H",
        "#define LHAT_JIT_STENCILS_H",
        "",
        "#include <stddef.h>",
        "",
        '#include "jit.h"',
        '#include "lhat/object.h"',
        "",
        "// What the stencils read, laid out where they run as where they "
        "were compiled.",
    ]
    for expression, value in zip(LAYOUT, layout):
        out.append(f'_Static_assert({expression} == {value}, '
                   f'"the stencils read {expression} as {value}");')
    out.append("")
    for name, (code, tail, holes) in stencils.items():
        out.append(f"static const uint8_t lhat_jit_code_{name}[] = {{")
        for i in range(0, len(code), 12):
            out.append("    " + " ".join(f"0x{b:02x}," for b in code[i:i + 12]))
        out.append("};")
        if holes:
            out.append(f"static const LhatJitHole lhat_jit_holes_{name}[] = {{")
            for (offset, kind, form, addend) in holes:
                out.append(f"    {{{offset}, {kind}, {form}, {addend}}},")
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
                        help="fail if a source changed since the header was "
                             "made; needs no Clang, writes nothing")
    args = parser.parse_args()

    if args.check:
        with open(OUTPUT, encoding="utf-8") as f:
            recorded = HASH_LINE.search(f.read())
        if recorded is None or recorded.group(1) != sources_hash():
            sys.exit(f"{os.path.relpath(OUTPUT, ROOT)} is out of date: "
                     "run jit/gen_stencils.py")
        return
    stencils = extract(*read_elf(compile_c(
        args.clang, os.path.join(ROOT, "jit", "stencils.c"))))
    text = render(stencils, measure_layout(args.clang))
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    main()
