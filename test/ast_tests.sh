#!/usr/bin/env bash
# Phase 1 AST tests.
#  valid   : test/ast/<name>.min  -> `minerva --dump-ast` must exit 0 and stdout must equal test/ast/<name>.ast
#  invalid : test/ast/invalid_*.min -> must exit 2 and must NOT print an AST
# Exits nonzero if any assertion fails.
set -u
EXE="${1:-./minerva}"
DIR="$(cd "$(dirname "$0")" && pwd)/ast"
pass=0
fail=0

report() { # name status detail
    if [ "$2" = PASS ]; then echo "PASS  $1"; pass=$((pass + 1));
    else echo "FAIL  $1  ($3)"; fail=$((fail + 1)); fi
}

for src in "$DIR"/*.min; do
    name="$(basename "$src" .min)"
    out="$(mktemp)"; out2="$(mktemp)"
    case "$name" in
    invalid_*)
        "$EXE" --dump-ast "$src" >"$out" 2>/dev/null
        rc=$?
        if [ "$rc" -ne 2 ]; then report "$name" FAIL "expected exit 2, got $rc"
        elif grep -q '^Program' "$out"; then report "$name" FAIL "AST printed for invalid input"
        else report "$name" PASS ""; fi
        ;;
    *)
        golden="$DIR/$name.ast"
        if [ ! -f "$golden" ]; then report "$name" FAIL "missing golden $golden"; rm -f "$out" "$out2"; continue; fi
        "$EXE" --dump-ast "$src" >"$out" 2>/dev/null
        rc=$?
        "$EXE" --dump-ast "$src" >"$out2" 2>/dev/null
        if [ "$rc" -ne 0 ]; then report "$name" FAIL "expected exit 0, got $rc"
        elif [ ! -s "$out" ]; then report "$name" FAIL "empty AST output"
        elif ! cmp -s "$out" "$golden"; then report "$name" FAIL "AST differs from golden"
        elif ! cmp -s "$out" "$out2"; then report "$name" FAIL "non-deterministic output"
        else report "$name" PASS ""; fi
        ;;
    esac
    rm -f "$out" "$out2"
done

echo "AST TESTS: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
