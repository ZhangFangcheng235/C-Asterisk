#!/usr/bin/env python3
# ============================================================
# caskide.py -- C* Studio on Linux / macOS
#
# The editor UI is the exact same web page as Windows (web/), so
# this file is only the little backend the page talks to: a
# 127.0.0.1 HTTP server that serves the page and implements the
# /api/* endpoints the Windows host (editor/webide.c) implements.
#
# It shells out to ./bin/caskc for check / build / run.  File
# dialogs are done with plain text prompts (stdin) so the page
# works in a browser without native widgets.
#
# Usage:
#   python3 caskide.py [file.cask]
#   -> prints the URL and opens your browser.
# ============================================================

import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import webbrowser
import secrets

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEBDIR = os.path.join(ROOT, "bin", "web")
if not os.path.isdir(WEBDIR):
    WEBDIR = os.path.join(ROOT, "web")
CASKC = os.path.join(ROOT, "bin", "caskc")
BUILD = os.path.join(ROOT, "build")
os.makedirs(BUILD, exist_ok=True)

OPENFILE = sys.argv[1] if len(sys.argv) > 1 else ""

# ---------------- running programs ----------------

RUNS = {}          # id -> dict(proc, out_chunks, done, code)
RUNS_LOCK = threading.Lock()
RUN_SEQ = [0]


class Handler(http.server.BaseHTTPRequestHandler):
    # --- quiet logging ---
    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n).decode("utf-8", "replace") if n else ""

    # --- static web page ---
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path.startswith("/api/"):
            return self.handle_api("GET", path, "")
        if path == "/" or path == "":
            path = "/index.html"
        fp = os.path.join(WEBDIR, path.lstrip("/"))
        # never leave the web folder
        if not os.path.realpath(fp).startswith(os.path.realpath(WEBDIR)):
            self.send_response(404)
            self.end_headers()
            return
        if not os.path.isfile(fp):
            self.send_response(404)
            self.end_headers()
            return
        ext = os.path.splitext(fp)[1].lower()
        ctype = {
            ".html": "text/html; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".js": "text/javascript; charset=utf-8",
            ".json": "application/json; charset=utf-8",
            ".svg": "image/svg+xml",
            ".png": "image/png",
        }.get(ext, "application/octet-stream")
        with open(fp, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        body = self._read_body()
        if path.startswith("/api/"):
            return self.handle_api("POST", path, body)
        self.send_response(405)
        self.end_headers()

    # ---------------- the API ----------------
    def handle_api(self, method, path, body):
        try:
            data = json.loads(body) if body else {}
        except Exception:
            data = {}

        if path == "/api/info":
            return self._json({"app": "C* Studio", "version": "0.4.2",
                               "open": OPENFILE})

        if path == "/api/dialog/open":
            p = input("打开文件路径: ").strip().strip('"')
            if p and os.path.isfile(p):
                return self._json({"path": p})
            return self._json({"cancelled": True})

        if path == "/api/dialog/saveas":
            p = input("保存到路径: ").strip().strip('"')
            if not p:
                return self._json({"cancelled": True})
            text = data.get("text", "")
            enc = data.get("encoding", "utf-8")
            self._save(p, text, enc)
            return self._json({"path": p})

        if path == "/api/load":
            p = data.get("path", "")
            text, enc = self._load(p)
            if text is None:
                return self._json({"error": "cannot read the file"})
            return self._json({"path": p, "text": text, "encoding": enc})

        if path == "/api/save":
            p = data.get("path", "")
            text = data.get("text", "")
            enc = data.get("encoding", "utf-8")
            ok = self._save(p, text, enc)
            return self._json({"ok": bool(ok)} if ok else {"error": "cannot write the file"})

        if path == "/api/enc":
            p = data.get("path", "")
            to = data.get("to", "utf-8")
            text, _ = self._load(p)
            if text is None:
                return self._json({"error": "cannot read the file"})
            self._save(p, text, to)
            return self._json({"text": text, "encoding": to})

        if path == "/api/check":
            rc, errs, _ = self._caskc_check(data)
            return self._json({"errs": errs, "rc": rc})

        if path == "/api/build" or path == "/api/run":
            ok, errs, log, exe, rid = self._build_run(data, run=(path == "/api/run"))
            if not ok:
                return self._json({"ok": False, "errs": errs, "log": log})
            if path == "/api/build":
                return self._json({"ok": True, "errs": errs, "log": log, "exe": exe})
            return self._json({"ok": True, "id": rid, "errs": errs, "log": log})

        if path.startswith("/api/output"):
            qd = self.path.split("?", 1)[1]
            rid = 0
            if "id=" in qd:
                rid = int(qd.split("id=", 1)[1].split("&", 1)[0])
            return self._output(rid)

        if path == "/api/stop":
            rid = data.get("id", 0)
            self._stop(rid)
            return self._json({"ok": True})

        if path == "/api/send":
            rid = data.get("id", 0)
            text = data.get("text", "")
            self._send(rid, text)
            return self._json({"ok": True})

        if path == "/api/title":
            # the browser tab title is set by the page itself; nothing to do
            return self._json({"ok": True})

        if path == "/api/reveal":
            p = data.get("path", "")
            try:
                subprocess.Popen(["xdg-open", os.path.dirname(p)])
            except Exception:
                pass
            return self._json({"ok": True})

        return self._json({"error": "unknown api"}, 404)

    # ---------------- helpers ----------------
    @staticmethod
    def _load(p):
        """Return (utf8_text, encoding) or (None, None)."""
        if not p:
            return None, None
        with open(p, "rb") as f:
            raw = f.read()
        try:
            return raw.decode("utf-8"), "utf-8"
        except UnicodeDecodeError:
            return raw.decode("gbk", "replace"), "gbk"

    @staticmethod
    def _save(p, text, enc):
        """Save with the same conventions as the Windows host: CRLF,
        utf-8 or gbk depending on the file's current encoding."""
        try:
            data = text.replace("\r\n", "\n").replace("\n", "\r\n")
            if enc and enc.lower() == "gbk":
                data = data.encode("gbk", "replace")
            else:
                data = data.encode("utf-8")
            with open(p, "wb") as f:
                f.write(data)
            return True
        except Exception:
            return False

    def _tmp_cask(self, text, nearp):
        """Write the buffer to a scratch .cask so `use "x.h"` still finds
        its headers; next to the real file when saved, else in build/."""
        RUN_SEQ[0] += 1
        d = os.path.dirname(nearp) if nearp else BUILD
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, "_caskstudio_%d.cask" % RUN_SEQ[0])
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return path

    @staticmethod
    def _parse_diag(text):
        """K|line|col|message lines (K = P/S/E/H) -> JSON err list."""
        errs = []
        for line in text.splitlines():
            if len(line) > 3 and line[0] in "PSEH" and line[1] == "|":
                parts = line.split("|", 3)
                if len(parts) == 4:
                    try:
                        errs.append({"k": parts[0], "line": int(parts[1]),
                                     "col": int(parts[2]), "msg": parts[3]})
                    except ValueError:
                        pass
        return errs

    def _caskc_check(self, data):
        text = data.get("text", "")
        nearp = data.get("path", "")
        tmp = self._tmp_cask(text, nearp)
        exe = os.path.join(BUILD, "_caskstudio_%d.chk" % RUN_SEQ[0])
        cmd = [CASKC, tmp, "--check", "--diag", "-o", exe]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        except Exception as e:
            return -1, [{"k": "E", "line": 1, "col": 1, "msg": str(e)}], ""
        out = r.stderr or ""
        os.remove(tmp)
        return r.returncode, self._parse_diag(out), out

    def _build_run(self, data, run=False):
        text = data.get("text", "")
        nearp = data.get("path", "")
        fast = data.get("fast", False)
        tmp = self._tmp_cask(text, nearp)
        base = os.path.basename(nearp) if nearp else "_caskstudio_%d" % RUN_SEQ[0]
        base = os.path.splitext(base)[0] or base
        exe = os.path.join(os.path.dirname(nearp) if nearp else BUILD, base)
        cmd = [CASKC, tmp, "-o", exe]
        if fast:
            cmd.append("--fast")
        cmd.append("--diag")
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        except Exception as e:
            return False, [{"k": "E", "line": 1, "col": 1, "msg": str(e)}], str(e), "", None
        errs = self._parse_diag(r.stderr or "")
        log = r.stderr or ""
        if r.returncode != 0:
            return False, errs, log, "", None

        if not run:
            return True, errs, log, exe, None

        # start the program and stream its output
        rid = RUN_SEQ[0]
        p = subprocess.Popen([exe], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                             text=True, bufsize=1)
        state = {"proc": p, "out": [], "done": False, "code": 0,
                 "reader": True}
        RUNS[rid] = state

        def reader():
            try:
                for line in iter(p.stdout.readline, ""):
                    with RUNS_LOCK:
                        state["out"].append(line)
                p.wait()
                with RUNS_LOCK:
                    state["code"] = p.returncode or 0
                    state["done"] = True
            except Exception:
                with RUNS_LOCK:
                    state["done"] = True

        threading.Thread(target=reader, daemon=True).start()
        return True, errs, log, exe, rid

    def _output(self, rid):
        st = RUNS.get(rid)
        out, done, code = "", False, 0
        if st:
            with RUNS_LOCK:
                out = "".join(st["out"])
                done = st["done"]
                code = st["code"]
        return self._json({"id": rid, "out": out, "done": done, "code": code})

    @staticmethod
    def _stop(rid):
        st = RUNS.get(rid)
        if st:
            p = st["proc"]
            try:
                p.terminate()
                p.wait(timeout=3)
            except Exception:
                pass
            with RUNS_LOCK:
                st["done"] = True

    @staticmethod
    def _send(rid, text):
        st = RUNS.get(rid)
        if st:
            try:
                st["proc"].stdin.write(text + "\n")
                st["proc"].stdin.flush()
            except Exception:
                pass


def find_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    port = find_port()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    url = "http://127.0.0.1:%d/" % port
    print("C* Studio  (Linux/macOS)")
    print("  editor: %s" % url)
    print("  serving web page from: %s" % WEBDIR)
    print("  compiler: %s" % CASKC)
    if OPENFILE:
        print("  opening: %s" % OPENFILE)
    print("  Ctrl+C to stop")
    threading.Thread(target=lambda: webbrowser.open(url), daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
