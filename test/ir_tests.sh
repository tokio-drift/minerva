#!/usr/bin/env bash
# Phase 3 IR generation, publication and verifier tests.
#  test/ir/manifest.tsv lists one case per line:
#      source file <TAB> expected exit status <TAB> expected diagnostic categories <TAB> golden file
#  A case with exit status 0 must publish an IR file that matches the golden file,
#  must be reproducible byte for byte and must pass the IR verifier.
#  A rejected case must not publish any output.
#  test/ir/malformed/*.ir must be rejected by the IR verifier.
# Exits nonzero if any assertion fails.
set -u
EXE="${1:-./minerva}"
DIR="$(cd "$(dirname "$0")" && pwd)/ir"
MANIFEST="$DIR/manifest.tsv"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0
fail=0

report() { # name status detail
    if [ "$2" = PASS ]; then echo "PASS  $1"; pass=$((pass + 1));
    else echo "FAIL  $1  ($3)"; fail=$((fail + 1)); fi
}

categories_of() { # file -> comma separated list of distinct diagnostic categories
    grep -o 'error\[[a-z-]*\]' "$1" | sed 's/error\[\(.*\)\]/\1/' | sort -u | paste -sd, -
}

while IFS=$'\t' read -r file expected_status expected_categories golden; do
    case "$file" in ''|\#*) continue ;; esac
    src="$DIR/$file"
    name="$(basename "$file" .min)"
    if [ ! -f "$src" ]; then report "$file" FAIL "missing source file"; continue; fi
    out="$WORK/$name.ir"
    again="$WORK/$name.again.ir"
    err="$WORK/$name.err"
    "$EXE" "$src" -o "$out" >/dev/null 2>"$err"
    rc=$?
    found="$(categories_of "$err")"
    if [ "$rc" -ne "$expected_status" ]; then
        report "$file" FAIL "expected exit $expected_status, got $rc"
    elif [ "$expected_status" = 0 ]; then
        if [ ! -s "$out" ]; then
            report "$file" FAIL "no IR file was published"
        elif ! cmp -s "$out" "$DIR/$golden"; then
            report "$file" FAIL "IR differs from the golden file"
        elif ! "$EXE" --verify-ir "$out" >/dev/null 2>&1; then
            report "$file" FAIL "the verifier rejected the published IR"
        elif grep -q 'TOKEN TABLE' "$out"; then
            report "$file" FAIL "the published IR contains a token table"
        else
            "$EXE" "$src" -o "$again" >/dev/null 2>&1
            if ! cmp -s "$out" "$again"; then
                report "$file" FAIL "the published IR is not reproducible"
            else
                report "$file" PASS ""
            fi
        fi
    elif [ -e "$out" ]; then
        report "$file" FAIL "an IR file was published for a rejected source"
    elif [ "$expected_categories" = "-" ]; then
        if [ -n "$found" ]; then
            report "$file" FAIL "unexpected diagnostics: $found"
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
done < "$MANIFEST"

# A rejected source must leave an existing output file byte for byte unchanged.
existing="$WORK/existing.ir"
printf 'previous content\n' > "$existing"
cp "$existing" "$WORK/existing.copy"
"$EXE" "$DIR/invalid_control_flow.min" -o "$existing" >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 4 ]; then
    report "atomicity-existing" FAIL "expected exit 4, got $rc"
elif ! cmp -s "$existing" "$WORK/existing.copy"; then
    report "atomicity-existing" FAIL "the existing output file was modified"
elif [ -n "$(find "$WORK" -name '.existing.ir.tmp*' -print -quit)" ]; then
    report "atomicity-existing" FAIL "a temporary file was left behind"
else
    report "atomicity-existing" PASS ""
fi

# A rejected source must not create an output file when there is none.
missing="$WORK/created.ir"
"$EXE" "$DIR/invalid_control_flow.min" -o "$missing" >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 4 ]; then
    report "atomicity-missing" FAIL "expected exit 4, got $rc"
elif [ -e "$missing" ]; then
    report "atomicity-missing" FAIL "an output file was created for a rejected source"
else
    report "atomicity-missing" PASS ""
fi

# Malformed IR must be rejected by the verifier.
for bad in "$DIR"/malformed/*.ir; do
    name="$(basename "$bad")"
    "$EXE" --verify-ir "$bad" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then report "malformed/$name" FAIL "the verifier accepted malformed IR";
    elif [ "$rc" -ne 4 ]; then report "malformed/$name" FAIL "expected exit 4, got $rc";
    else report "malformed/$name" PASS ""; fi
done

# A published IR file must pass the verifier.
for good in "$DIR"/valid_*.ir; do
    name="$(basename "$good")"
    "$EXE" --verify-ir "$good" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then report "verify/$name" FAIL "expected exit 0, got $rc";
    else report "verify/$name" PASS ""; fi
done

echo "IR TESTS: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
