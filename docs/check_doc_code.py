#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_doc_code.py -- compile every ```cask block in a Markdown file.

Usage:  python check_doc_code.py docs/小学版教程.md
Blocks that look like a whole program (they contain 'main') are compiled with
bin/caskc.exe.  Blocks that need keyboard input are compiled but not run.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CASK = os.path.join(ROOT, "bin", "caskc.exe")


def blocks(path):
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    out, i, cur = [], 0, None
    while i < len(lines):
        s = lines[i].strip()
        if s.startswith("```"):
            if cur is None:
                lang = s[3:].strip()
                cur = (lang, [], i + 1)
            else:
                out.append((cur[0], "\n".join(cur[1]), cur[2]))
                cur = None
            i += 1
            continue
        if cur is not None:
            cur[1].append(lines[i])
        i += 1
    return out


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "小学版教程.md")
    tmp = tempfile.mkdtemp(prefix="caskdocs")
    n_ok = n_skip = n_bad = 0
    for idx, (lang, code, line) in enumerate(blocks(src)):
        if lang != "cask":
            continue
        if "main" not in code and "start" not in code:
            n_skip += 1
            continue          # a fragment, not a whole program
        path = os.path.join(tmp, "b%02d.cask" % idx)
        with open(path, "w", encoding="utf-8", newline="\r\n") as f:
            f.write(code + "\n")
        exe = os.path.join(tmp, "b%02d.exe" % idx)
        p = subprocess.run([CASK, path, "-o", exe],
                           capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if p.returncode != 0:
            n_bad += 1
            print("=== block at %s line %d FAILED to compile ===" % (src, line))
            print(code)
            print(p.stdout.strip())
            print(p.stderr.strip())
            continue
        if "C_OPED.cash" in code:
            # a window program: build it, but never run it (it would open
            # a real window and wait inside run() forever)
            print("[ok]   %s line %d (window program, built but not run)" % (os.path.basename(src), line))
            n_ok += 1
            continue
        if "input" in code:
            print("[ok]   %s line %d (compiled, needs input so not run)" % (os.path.basename(src), line))
            n_ok += 1
            continue
        r = subprocess.run([exe], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if r.returncode != 0:
            n_bad += 1
            print("=== block at line %d FAILED at run time ===" % line)
            print(code)
            print(r.stdout, r.stderr)
        else:
            print("[ok]   %s line %d" % (os.path.basename(src), line))
            n_ok += 1
    print("---- %d ok, %d skipped (fragments), %d bad ----" % (n_ok, n_skip, n_bad))
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
