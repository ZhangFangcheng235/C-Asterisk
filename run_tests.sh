#!/usr/bin/env bash
# ============================================================
#  run_tests.sh -- compiles every tests/*.cask and compares the
#  program output with tests/*.expected (byte for byte).
#
#  conventions:
#    tests/X.cask     the program
#    tests/X.expected exact expected output
#    tests/X.in       optional input piped to the program
#    tests/rtcheck_*  must FAIL at run time (runtime check fired)
#
#  The native\* window-header tests are skipped on Linux unless
#  GTK3 is available, because they build against the GUI toolkit.
# ============================================================
set -u
cd "$(dirname "$0")"

CASKC="./bin/caskc"
if [ ! -x "$CASKC" ]; then
  echo "caskc not built. Run ./build.sh first."
  exit 1
fi

mkdir -p build
pass=0
fail=0

# ---- console tests: tests/*.cask ----
run_one() {
  local n="$1"
  local cask="tests/$n.cask"
  local exp="tests/$n.expected"
  if [ ! -f "$exp" ]; then
    echo "[skip] $n - no expected file"
    fail=$((fail+1)); return
  fi

  if [[ "$n" == cerr_* ]]; then
    # the compiler itself must reject the program
    "$CASKC" "$cask" -o "build/$n.out.bin" >/dev/null 2>"build/$n.cerr"
    local rc=$?
    if [ $rc -eq 0 ]; then
      echo "[FAIL] $n - the compiler accepted a program it should reject"
      fail=$((fail+1)); return
    fi
    if cmp -s "build/$n.cerr" "$exp"; then
      echo "[ok]   $n"; pass=$((pass+1))
    else
      echo "[FAIL] $n - the compiler's message differs:"
      diff "build/$n.cerr" "$exp" || true
      fail=$((fail+1))
    fi
    return
  fi

  local wantfail=0
  [[ "$n" == rtcheck_* ]] && wantfail=1

  "$CASKC" "$cask" -o "build/$n" >/dev/null 2>"build/$n.cerr"
  if [ $? -ne 0 ]; then
    echo "[FAIL] $n - could not compile:"
    cat "build/$n.cerr"
    fail=$((fail+1)); return
  fi

  local rc=0
  if [ -f "tests/$n.in" ]; then
    ./build/"$n" < "tests/$n.in" > "build/$n.out" 2>&1 || rc=$?
  else
    ./build/"$n" > "build/$n.out" 2>&1 || rc=$?
  fi

  if ! cmp -s "build/$n.out" "$exp"; then
    echo "[FAIL] $n - output differs:"
    diff "build/$n.out" "$exp" || true
    fail=$((fail+1)); return
  fi
  if [ "$wantfail" -eq 1 ]; then
    [ "$rc" -eq 0 ] && { echo "[FAIL] $n - a runtime error was expected, but it succeeded"; fail=$((fail+1)); return; }
  else
    [ "$rc" -ne 0 ] && { echo "[FAIL] $n - exit code $rc was not 0"; fail=$((fail+1)); return; }
  fi
  echo "[ok]   $n"; pass=$((pass+1))
}

for cask in tests/*.cask; do
  [ -e "$cask" ] || continue
  n="$(basename "$cask" .cask)"
  run_one "$n"
done

# ---- native window-header tests: tests/native/*.cask ----
#  need GTK3 on Linux; skip the GUI ones if it is not present.
if [ -d tests/native ]; then
  gtk_ok=0
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists gtk+-3.0; then gtk_ok=1; fi
  for cask in tests/native/*.cask; do
    [ -e "$cask" ] || continue
    n="$(basename "$cask" .cask)"
    if [ "$gtk_ok" -eq 0 ]; then
      echo "[skip] native/$n - GTK3 dev not installed (apt install libgtk-3-dev)"
      continue
    fi
    if [[ "$n" == cerr_* ]]; then
      "$CASKC" "$cask" --console -o "build/nat_$n.bin" >/dev/null 2>"build/nat_$n.cerr"
      if [ $? -eq 0 ]; then
        echo "[FAIL] native/$n - the compiler accepted a program it should reject"
        fail=$((fail+1)); continue
      fi
      if cmp -s "build/nat_$n.cerr" "tests/native/$n.expected"; then
        echo "[ok]   native/$n"; pass=$((pass+1))
      else
        echo "[FAIL] native/$n - the compiler's message differs:"
        diff "build/nat_$n.cerr" "tests/native/$n.expected" || true
        fail=$((fail+1))
      fi
      continue
    fi
    "$CASKC" "$cask" --console -o "build/nat_$n" >/dev/null 2>"build/nat_$n.cerr"
    if [ $? -ne 0 ]; then
      echo "[FAIL] native/$n - could not compile:"
      cat "build/nat_$n.cerr"
      fail=$((fail+1)); continue
    fi
    # window programs block on the event loop; the console tests print a
    # few lines and then exit, so run them with a timeout.
    if timeout 10 ./build/"nat_$n" > "build/nat_$n.out" 2>&1; then :; fi
    if cmp -s "build/nat_$n.out" "tests/native/$n.expected"; then
      echo "[ok]   native/$n"; pass=$((pass+1))
    else
      echo "[FAIL] native/$n - output differs:"
      diff "build/nat_$n.out" "tests/native/$n.expected" || true
      fail=$((fail+1))
    fi
  done
fi

echo
echo "================================================"
echo " passed: $pass    failed: $fail"
echo "================================================"
[ "$fail" -eq 0 ]
