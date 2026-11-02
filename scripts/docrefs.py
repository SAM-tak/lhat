#!/usr/bin/env python3
"""Check the labels of the design documents and every reference to them.

    python scripts/docrefs.py

Each numbered heading ends with its label -- `### 8.8 ホストのデータ
[modules.hostdata]` -- and a reference is that label in brackets, in a
comment or in a document's prose (DesignDocuments/README.md). A document's own
name (`[modules]`) refers to the whole of it.
"""

import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, 'DesignDocuments')
NAMES = {'01': 'lex', '02': 'syntax', '03': 'pipeline', '04': 'errors',
         '05': 'modules', '07': 'lsp', '08': 'lton', '09': 'debug',
         '10': 'l10n', '11': 'binary', '12': 'spec'}
SOURCES = ['*.c', '*.h', '*.cpp', '*.lh', '*.py', '*.cmake', 'CMakeLists.txt',
           '*.ts', '*.mjs']

NUMBERED = re.compile(r'^#{2,6} \d+(?:\.\d+)*(?:\+\d*)?\.?(?=\s|$)')
LABEL = re.compile(r'\[([a-z0-9]+(?:\.[a-z0-9-]+)+)\]\s*$')
REF = re.compile(r'\[((?:' + '|'.join(NAMES.values()) + r')(?:\.[a-z0-9-]+)*)\]')
# What references were written as before the labels.
OLD = re.compile(r'(?<![\d.])\d\d の \d|\d\s?章|(?<![A-Za-z\d])\d+(?:\.\d+)*改|'
                 r'§|Section\s\d')


def labels(report):
    """Every label, after checking that each numbered heading has one of its
    own and that it starts with its document's name."""
    found = set(NAMES.values())
    for name in sorted(os.listdir(DOCS)):
        m = re.match(r'(\d\d)-.*\.md$', name)
        if not m:
            continue
        fence = False
        numbers = set()
        for i, line in enumerate(io.open(os.path.join(DOCS, name), encoding='utf-8')):
            fence ^= line.startswith('```')
            if fence or not NUMBERED.match(line):
                continue
            at = f'DesignDocuments/{name}:{i + 1}'
            number = line.split()[1].rstrip('.')
            if number in numbers:
                report(f'{at}: {number} is already another heading\'s number')
            numbers.add(number)
            label = LABEL.search(line)
            if not label:
                report(f'{at}: a numbered heading without a label')
            elif not label.group(1).startswith(NAMES.get(m.group(1), '?') + '.'):
                report(f'{at}: [{label.group(1)}] does not start with {NAMES.get(m.group(1))}.')
            elif label.group(1) in found:
                report(f'{at}: [{label.group(1)}] is already another heading\'s')
            else:
                found.add(label.group(1))
    return found


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
    """A document outside fenced code."""
    fence = False
    for i, line in enumerate(lines):
        if line.startswith('```'):
            fence = not fence
        elif not fence:
            yield i, 0, len(line)


def check(path, spans, known, report):
    lines = io.open(os.path.join(ROOT, path), encoding='utf-8').read().split('\n')
    for i, a, b in spans(lines):
        text = lines[i][a:b]
        old = OLD.search(text)
        if old:
            report(f'{path}:{i + 1}: old spelling: {old.group(0)}')
        for m in REF.finditer(text):
            if m.group(1) not in known:
                report(f'{path}:{i + 1}: [{m.group(1)}] names no heading')


def main():
    problems = []
    report = problems.append
    known = labels(report)
    for name in sorted(os.listdir(DOCS)):
        if re.match(r'\d\d-.*\.md$', name):   # README.md spells the notation out
            check('DesignDocuments/' + name, prose, known, report)
    listed = subprocess.run(['git', 'ls-files', '--', *SOURCES], cwd=ROOT,
                            capture_output=True, text=True, encoding='utf-8')
    for path in listed.stdout.splitlines():
        if path.startswith(('vendor/', 'node_modules/')):
            continue
        spans = c_comments if path.endswith(('.c', '.h', '.cpp', '.ts', '.mjs')) \
            else hash_comments
        check(path, spans, known, report)
    for line in problems:
        print(line)
    print(f'{len(problems)} problem(s)' if problems else 'every reference names a heading')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
