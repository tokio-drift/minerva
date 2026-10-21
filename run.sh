#!/usr/bin/env bash
# Runs every test suite: the source files under test/, the golden AST tests and
# the semantic analysis tests. Exits nonzero if any suite fails.
set -u
EXE="${1:-./minerva}"
HERE="$(cd "$(dirname "$0")" && pwd)"
pass=0
fail=0
GREEN="\033[1;32m"
RED="\033[1;31m"
CYAN="\033[1;36m"
ENDCLR="\033[0m"

run_suite() { # label command...
    local label="$1"; shift
    echo -e "${CYAN}${label}${ENDCLR}"
    if "$@"; then
        echo -e "${GREEN}Result: PASS${ENDCLR}\n"
        pass=$((pass + 1))
    else
        echo -e "${RED}Result: FAIL${ENDCLR}\n"
        fail=$((fail + 1))
    fi
}

# Frontend tests: the files under test/ are only parsed, so the two error cases
# are expected to fail with status 2.
frontend_suite() {
    local status=0
    for f in "$HERE"/test/*.min; do
        echo "Running: ${f#"$HERE"/}"
        "$EXE" "$f" >/dev/null 2>&1
        local code=$?
        if [[ "$f" == *"lexical_errors"* || "$f" == *"syntax_errors"* ]]; then
            if [ "$code" -eq 2 ]; then
                echo -e "Result: ${GREEN}PASS${ENDCLR}"
            else
                echo -e "Result: ${RED}FAIL${ENDCLR} (expected status 2, got $code)"
                status=1
            fi
        else
            if [ "$code" -eq 0 ]; then
                echo -e "Result: ${GREEN}PASS${ENDCLR}"
            else
                echo -e "Result: ${RED}FAIL${ENDCLR} (expected status 0, got $code)"
                status=1
            fi
        fi
        echo
    done
    return $status
}

run_suite "FRONTEND TESTS" frontend_suite
run_suite "AST TESTS" bash "$HERE/test/ast_tests.sh" "$EXE"
run_suite "SEMANTIC TESTS" bash "$HERE/test/semantic_tests.sh" "$EXE"

echo -e "${CYAN}TEST RESULTS${ENDCLR}"
echo -e "${GREEN}Suites passed${ENDCLR}: $pass"
echo -e "${RED}Suites failed${ENDCLR}: $fail"
[ "$fail" -eq 0 ]
