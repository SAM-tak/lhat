#!/usr/bin/env python3
"""Check that every reference to a design document names a heading.

    python scripts/docrefs.py

A reference is `NN§S` (DesignDocuments/README.md): NN is the two-digit prefix
of a DesignDocuments file and S a heading's number -- `8.8`, a chapter `14`, a
revision `15.1+` or `15.1+2`. `NN§` alone is the whole document. Without NN it
means 02 in code and the document itself inside a document.
"""

import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, 'DesignDocuments')
DEFAULT = '02'
SOURCES = ['*.c', '*.h', '*.cpp', '*.lh', '*.py', '*.cmake', 'CMakeLists.txt',
           '*.ts', '*.mjs']

SEC = r'\d+(?:\.\d+)*(?:\+\d*)?'
HEADING = re.compile(r'^#{2,6} (' + SEC + r')\.?(?=\s|$)')
REF = re.compile(r'(?<![\w.])(\d\d)?§(' + SEC + r')?')
# What references were written as before the notation.
OLD = re.compile(r'(?<![\d.])\d\d の \d|\d\s?章|(?<![A-Za-z\d])\d+(?:\.\d+)*改|'
                 r'§\s\d|\d\d\s§|Section\s\d')


def headings():
    docs = {}
    for name in sorted(os.listdir(DOCS)):
        m = re.match(r'(\d\d)-.*\.md$', name)
        if not m:
            continue
        found = set()
        fence = False
        for line in io.open(os.path.join(DOCS, name), encoding='utf-8'):
            fence ^= line.startswith('```')
            h = None if fence else HEADING.match(line)
            if h:
                found.add(h.group(1))
        docs[m.group(1)] = found
    return docs


def c_comments(lines):
    """(line, start, end) of comment text in C and the languages like it."""
    block = False
    for i, line in enumerate(lines):
        j, start, quote = 0, 0 if block else None, None
        while j < len(line):
            if block:
                if line.startswith('*/', j):
                    yield i, start, j
                    block, start = False, None
                    j += 2
                    continue
            elif quote:
                if line[j] == '\\':
                    j += 1
                elif line[j] == quote:
                    quote = None
            elif line[j] in '"\'':
                quote = line[j]
            elif line.startswith('//', j):
                yield i, j, len(line)
                break
            elif line.startswith('/*', j):
                block, start = True, j
                j += 1
            j += 1
        else:
            if block:
                yield i, start, len(line)


def hash_comments(lines):
    for i, line in enumerate(lines):
        quote = None
        for j, ch in enumerate(line):
            if quote:
                quote = None if ch == quote else quote
            elif ch in '"\'':
                quote = ch
            elif ch == '#':
                yield i, j, len(line)
                break


def prose(lines):
    """A document outside fenced code, less each heading's own number."""
    fence = False
    for i, line in enumerate(lines):
        if line.startswith('```'):
            fence = not fence
        elif not fence:
            h = HEADING.match(line)
            yield i, h.end() if h else 0, len(line)


def check(path, home, spans, docs):
    lines = io.open(os.path.join(ROOT, path), encoding='utf-8').read().split('\n')
    bad = 0
    for i, a, b in spans(lines):
        text = lines[i][a:b]
        old = OLD.search(text)
        if old:
            print(f'{path}:{i + 1}: old spelling: {old.group(0)}')
            bad += 1
        for m in REF.finditer(text):
            doc = m.group(1) or home
            ok = (doc in docs if m.group(2) is None
                  else m.group(2) in docs.get(doc, ()))
            if not ok:
                print(f'{path}:{i + 1}: {m.group(0)} names no heading')
                bad += 1
    return bad


def main():
    docs = headings()
    bad = 0
    for name in sorted(os.listdir(DOCS)):
        m = re.match(r'(\d\d)-.*\.md$', name)
        if m:   # README.md spells the notation out, examples and all
            bad += check('DesignDocuments/' + name, m.group(1), prose, docs)
    listed = subprocess.run(['git', 'ls-files', '--', *SOURCES], cwd=ROOT,
                            capture_output=True, text=True, encoding='utf-8')
    for path in listed.stdout.splitlines():
        if path.startswith(('vendor/', 'node_modules/')):
            continue
        spans = c_comments if path.endswith(('.c', '.h', '.cpp', '.ts', '.mjs')) \
            else hash_comments
        bad += check(path, DEFAULT, spans, docs)
    print(f'{bad} reference(s) to fix' if bad else 'every reference names a heading')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
