#!/usr/bin/env python3
"""Interface translations of VisualTC.

The source code is written in Brazilian Portuguese; resources/i18n/<lang>.json
maps each source text to its translation. This script lists the texts of the
code (tr(), QObject::tr(), QCoreApplication::translate()) plus the messages of
the core library (resources/i18n/core_messages.txt) and checks that every
language file translates all of them.

    tools/i18n.py check              # exit 1 if something is missing (CTest)
    tools/i18n.py missing es         # list what es.json still lacks
    tools/i18n.py list               # every source text, one JSON string per line
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
I18N = os.path.join(ROOT, "resources", "i18n")
LANGS = ["es", "en"]

CALL = re.compile(r"(?<![A-Za-z0-9_])(?:QObject::|QCoreApplication::)?(tr|translate)\s*\(")
LITERAL = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}


def unescape(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            out.append(ESCAPES.get(body[i + 1], body[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def literal_at(text, pos):
    """Adjacent string literals starting at pos (C++ concatenation)."""
    parts, end = [], pos
    while True:
        m = LITERAL.match(text, end)
        if not m:
            break
        parts.append(unescape(m.group(1)))
        end = m.end()
    return ("".join(parts), end) if parts else (None, pos)


def strip_comments(text):
    # Keep string literals intact while removing // and /* */ comments.
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text[i] == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def source_texts():
    found = {}
    for base, _, files in os.walk(SRC):
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(base, name)
            with open(path, encoding="utf-8") as f:
                text = strip_comments(f.read())
            for m in CALL.finditer(text):
                pos = m.end()
                if m.group(1) == "translate":
                    ctx, pos = literal_at(text, pos)
                    if ctx is None:
                        continue
                    comma = re.compile(r"\s*,").match(text, pos)
                    if not comma:
                        continue
                    pos = comma.end()
                s, _ = literal_at(text, pos)
                if s:
                    found.setdefault(s, os.path.relpath(path, ROOT))
    with open(os.path.join(I18N, "core_messages.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                found.setdefault(line.replace("\\n", "\n"), "core_messages.txt")
    return found


def load(lang):
    with open(os.path.join(I18N, lang + ".json"), encoding="utf-8") as f:
        return json.load(f)


def placeholders(text):
    return set(re.findall(r"%\d|%n", text))


def missing(lang, texts):
    """Texts without a translation, or whose translation lost/added a %1/%n."""
    table = load(lang)
    out = []
    for s in texts:
        t = table.get(s, "")
        if not t or placeholders(s) != placeholders(t):
            out.append(s)
    return out


def main(argv):
    texts = source_texts()
    cmd = argv[1] if len(argv) > 1 else "check"
    if cmd == "list":
        for s in sorted(texts):
            print(json.dumps(s, ensure_ascii=False))
        return 0
    if cmd == "missing":
        for s in missing(argv[2], texts):
            print(json.dumps(s, ensure_ascii=False))
        return 0
    bad = 0
    for lang in LANGS:
        miss = missing(lang, texts)
        if miss:
            bad += len(miss)
            print(f"{lang}.json: {len(miss)} texto(s) sem tradução, por exemplo:")
            for s in miss[:15]:
                print("   ", json.dumps(s, ensure_ascii=False), "—", texts[s])
    print(f"{len(texts)} textos; {'faltam ' + str(bad) if bad else 'todos traduzidos'} ({', '.join(LANGS)}).")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
