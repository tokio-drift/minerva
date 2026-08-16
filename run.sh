#!/usr/bin/env bash
EXE="${1:-./lexer}"

for f in test/*.min; do
    echo "Running: $f"
    "$EXE" "$f"
    echo
done