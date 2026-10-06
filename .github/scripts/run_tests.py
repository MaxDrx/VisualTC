#!/usr/bin/env python3
"""Runs ctest and, when tests fail, publishes the details of each failure as
GitHub annotations, so they can be read on the run page (and through the API)
without opening the raw log.

    python3 .github/scripts/run_tests.py --preset linux-release [ctest args...]
"""
import re
import subprocess
import sys

MAX_ANNOTATIONS = 9          # GitHub shows at most 10 errors per step
MAX_MESSAGE = 3800
# Lines that explain a failure (assertions, sanitizer reports, crashes);
# library chatter such as GDCM's "Warning: In ..." is left out when the
# output is too long.
KEYWORDS = re.compile(
    r"FAILED|FAIL!|Actual|Expected|Loc:|REQUIRE|CHECK|with expansion|with message|due to|"
    r"exception|Exception|what\(\)|runtime error|AddressSanitizer|LeakSanitizer|"
    r"UndefinedBehaviorSanitizer|SUMMARY:|#\d+ 0x|QFATAL|Segmentation|Timeout|"
    r"[Aa]ssertion|terminate called",
)
RESULT = re.compile(r"^\s*\d+/\d+ Test\s+#\d+: (\S+) .*(\*\*\*|Failed|Exception|Timeout)")
BOUNDARY = re.compile(r"^\s*(\d+/\d+ Test\s+#\d+:|Start\s+\d+:|\d+% tests passed)")


def escape(text: str, prop: bool = False) -> str:
    text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    if prop:
        text = text.replace(":", "%3A").replace(",", "%2C")
    return text


def failure_blocks(lines):
    blocks = []
    i = 0
    while i < len(lines):
        m = RESULT.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        j = i + 1
        while j < len(lines) and not BOUNDARY.match(lines[j]):
            j += 1
        blocks.append((name, lines[i + 1:j]))
        i = j
    return blocks


def summarize(block):
    if sum(len(line) + 1 for line in block) <= MAX_MESSAGE:
        return "\n".join(block)
    keep = set(range(max(0, len(block) - 12), len(block)))  # how it ended
    for k, line in enumerate(block):
        if KEYWORDS.search(line):
            keep.update(range(max(0, k - 2), min(len(block), k + 4)))
    picked = [block[k] for k in sorted(keep)]
    text = "\n".join(picked)
    return text[:MAX_MESSAGE] + ("\n[...]" if len(text) > MAX_MESSAGE else "")


def main() -> int:
    cmd = ["ctest", *sys.argv[1:], "--output-on-failure"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    lines = []
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        lines.append(line.rstrip("\r\n"))
    rc = proc.wait()
    if rc != 0:
        blocks = failure_blocks(lines)
        for name, block in blocks[:MAX_ANNOTATIONS]:
            print(f"::error title={escape('Teste falhou: ' + name, True)}::{escape(summarize(block))}")
        if not blocks:
            tail = "\n".join(lines[-60:])
            print(f"::error title={escape('ctest falhou', True)}::{escape(tail[-MAX_MESSAGE:])}")
        sys.stdout.flush()
    return rc


if __name__ == "__main__":
    sys.exit(main())
