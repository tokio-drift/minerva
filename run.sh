#!/usr/bin/env bash
# Generate IR and typed AST outputs without creating any log files.
# Usage: ./run.sh [generate|ir|ast|clean]
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-generate}"
EXE="${MINERVA_BIN:-$ROOT/minerva}"
TEST_DIR="$ROOT/test"
IR_DIR="$ROOT/ir"
AST_DIR="$ROOT/typed_ast"

case "$MODE" in
  generate|all|ir|ast|clean) ;;
  *)
    echo "Usage: $0 [generate|ir|ast|clean]" >&2
    exit 2
    ;;
esac

# Remove output folders entirely; this script never creates or writes logs.
if [[ "$MODE" == "clean" ]]; then
  rm -rf "$IR_DIR" "$AST_DIR" "$ROOT/generated"
  echo "Removed IR and typed AST output folders."
  exit 0
fi

# Build from lexer/parser sources if the compiler executable is absent.
if [[ ! -x "$EXE" ]]; then
  echo "Compiler executable missing; building from src/lexer.l and src/parser.y..."
  if ! make -C "$ROOT" minerva; then
    echo "Build failed; see the compiler errors above." >&2
    exit 2
  fi
fi

shopt -s nullglob
SOURCES=("$TEST_DIR"/*.min)
shopt -u nullglob
if [[ ${#SOURCES[@]} -ne 8 ]]; then
  echo "Expected exactly 8 test programs in $TEST_DIR; found ${#SOURCES[@]}." >&2
  exit 2
fi

mkdir -p "$IR_DIR" "$AST_DIR"
if [[ "$MODE" == "generate" || "$MODE" == "all" || "$MODE" == "ir" ]]; then
  rm -f "$IR_DIR"/*.ir
fi
if [[ "$MODE" == "generate" || "$MODE" == "all" || "$MODE" == "ast" ]]; then
  rm -f "$AST_DIR"/*.typed.ast
fi

pass=0
fail=0
for SRC in "${SOURCES[@]}"; do
  BASE="$(basename "${SRC%.min}")"
  FILE_OK=1
  printf '\n== %s ==\n' "$(basename "$SRC")"

  if [[ "$MODE" == "generate" || "$MODE" == "all" || "$MODE" == "ir" ]]; then
    if "$EXE" "$SRC" -o "$IR_DIR/$BASE.ir"; then
      echo "IR generated: $IR_DIR/$BASE.ir"
    else
      echo "IR generation FAILED for $SRC" >&2
      rm -f "$IR_DIR/$BASE.ir"
      FILE_OK=0
    fi
  fi

  if [[ "$MODE" == "generate" || "$MODE" == "all" || "$MODE" == "ast" ]]; then
    if "$EXE" --dump-typed-ast "$SRC" > "$AST_DIR/$BASE.typed.ast"; then
      echo "Typed AST generated: $AST_DIR/$BASE.typed.ast"
    else
      echo "Typed AST generation FAILED for $SRC" >&2
      rm -f "$AST_DIR/$BASE.typed.ast"
      FILE_OK=0
    fi
  fi

  if [[ "$FILE_OK" -eq 1 ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
  fi
done

echo
echo "Test programs successful: $pass/8"
if [[ "$MODE" == "generate" || "$MODE" == "all" || "$MODE" == "ir" ]]; then
  echo "IR files: $(find "$IR_DIR" -maxdepth 1 -type f -name '*.ir' | wc -l | tr -d ' ')"
fi
if [[ "$MODE" == "generate" || "$MODE" == "all" || "$MODE" == "ast" ]]; then
  echo "Typed AST files: $(find "$AST_DIR" -maxdepth 1 -type f -name '*.typed.ast' | wc -l | tr -d ' ')"
fi
[[ "$fail" -eq 0 ]]
