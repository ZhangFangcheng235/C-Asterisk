#!/usr/bin/env bash
# ============================================================
#  build.sh -- builds the C* (C Asterisk) toolchain on Linux / macOS
#  Needs: gcc (cc) in PATH; for window programs, GTK3 dev headers
#         (pkg-config --exists gtk+-3.0) on the build host.
# ============================================================
set -e
cd "$(dirname "$0")"

if ! command -v gcc >/dev/null 2>&1 && ! command -v cc >/dev/null 2>&1; then
  echo "[build] ERROR: no C compiler found."
  echo "        Install gcc / build-essential (Ubuntu: sudo apt install build-essential)."
  exit 1
fi

CC=gcc
command -v gcc >/dev/null 2>&1 || CC=cc

mkdir -p bin build

echo "[build] compiler  -> bin/caskc"
$CC -O2 -Wall -Wextra -o bin/caskc compiler/caskc.c

# GTK3 is only needed to build window programs; warn, don't fail.
if command -v pkg-config >/dev/null 2>&1; then
  if ! pkg-config --exists gtk+-3.0; then
    echo "[build] note: GTK3 dev not found -- window programs (C_OPED.cash)"
    echo "        will not compile until you install libgtk-3-dev."
  fi
fi

# The editor host (webide.c / wv2.cpp) is Windows-only (Win32 + WebView2).
# On Linux / macOS the editor is the same web page served by caskide.py.
if [ ! -f caskide.py ]; then
  echo "[build] ERROR: caskide.py is missing (Linux editor backend)."
  exit 1
fi
cp caskide.py bin/caskide.py
chmod +x bin/caskide.py

# a thin launcher so "run the editor" reads the same on both OSes
cat > bin/caskide.sh <<'EOF'
#!/usr/bin/env bash
# C* Studio on Linux / macOS: the same web page as Windows, served by
# a tiny local Python server, opened in your browser.
cd "$(dirname "$0")/.."
python3 bin/caskide.py "$@"
EOF
chmod +x bin/caskide.sh

# the page the editor shows
mkdir -p bin/web
for f in index.html style.css editor.js app.js blocks.js syntax.js header-info.json; do
  cp web/$f bin/web/$f
done

# runtime header + window / css headers
cp compiler/cask_rt.h bin/cask_rt.h
cp compiler/C_OPED.cash bin/C_OPED.cash
cp compiler/C_CSS.cash bin/C_CSS.cash
# never let a stale copy in build/ shadow the bin/ headers
rm -f build/C_OPED.cash build/C_CSS.cash examples/C_OPED.cash examples/C_CSS.cash 2>/dev/null || true

echo "[build] done.  Try:  ./bin/caskc examples/hello.cask --run"
echo "        or the editor:  ./bin/caskide.sh"
