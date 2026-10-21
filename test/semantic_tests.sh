#!/usr/bin/env bash
# Phase 2 semantic analysis tests.
#  test/semantic/manifest.tsv lists one case per line:
#      source file <TAB> expected exit status <TAB> expected diagnostic categories
#  Exit status 0 means the program must be accepted, 3 means it must be rejected
#  with exactly the listed diagnostic categories.
# Exits nonzero if any assertion fails.
set -u
EXE="${1:-./minerva}"
DIR="$(cd "$(dirname "$0")" && pwd)/semantic"
MANIFEST="$DIR/manifest.tsv"
pass=0
fail=0

report() { # name status detail
    if [ "$2" = PASS ]; then echo "PASS  $1"; pass=$((pass + 1));
    else echo "FAIL  $1  ($3)"; fail=$((fail + 1)); fi
}

categories_of() { # file -> comma separated list of distinct diagnostic categories
    grep -o 'error\[[a-z-]*\]' "$1" | sed 's/error\[\(.*\)\]/\1/' | sort -u | paste -sd, -
}

while IFS=$'\t' read -r file expected_status expected_categories; do
    case "$file" in ''|\#*) continue ;; esac
    src="$DIR/$file"
    if [ ! -f "$src" ]; then report "$file" FAIL "missing source file"; continue; fi
    out="$(mktemp)"; err="$(mktemp)"
    "$EXE" --semantic "$src" >"$out" 2>"$err"
    rc=$?
    found="$(categories_of "$err")"
    if [ "$rc" -ne "$expected_status" ]; then
        report "$file" FAIL "expected exit $expected_status, got $rc"
    elif [ "$expected_categories" = "-" ]; then
        if [ -n "$found" ]; then
            report "$file" FAIL "unexpected diagnostics: $found"
        elif ! grep -q 'semantic analysis: no errors' "$out"; then
            report "$file" FAIL "missing acceptance report"
        else
            report "$file" PASS ""
        fi
    else
        expected="$(echo "$expected_categories" | tr ',' '\n' | sort -u | paste -sd, -)"
        if [ "$found" != "$expected" ]; then
            report "$file" FAIL "expected [$expected], got [$found]"
        else
            report "$file" PASS ""
        fi
    fi
    rm -f "$out" "$err"
done < "$MANIFEST"

echo "SEMANTIC TESTS: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
