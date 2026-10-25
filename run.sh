
#!/usr/bin/env bash
# Usage: ./run.sh [ast|clean]
# Default: generate IR only.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-ir}"
EXE="${MINERVA_BIN:-$ROOT/minerva}"
TEST_DIR="$ROOT/test"
IR_DIR="$ROOT/ir"
AST_DIR="$ROOT/typed_ast"

case "$MODE" in
  ir|ast|clean) ;;
  *)
    echo "Usage: ./run.sh [ast|clean]" >&2
    exit 2
    ;;
esac

if [[ "$MODE" == "clean" ]]; then
  rm -rf "$IR_DIR" "$AST_DIR"
  echo "Generated output folders removed."
  exit 0
fi

if [[ ! -x "$EXE" ]]; then
  echo "Compiler executable missing. Building compiler..."
  if ! make -C "$ROOT"; then
    echo "Build failed." >&2
    exit 2
  fi
fi

shopt -s nullglob
SOURCES=("$TEST_DIR"/*.min)
shopt -u nullglob

if [[ ${#SOURCES[@]} -ne 25 ]]; then
  echo "Expected exactly 25 .min test programs in $TEST_DIR; found ${#SOURCES[@]}." >&2
  exit 2
fi

if [[ "$MODE" == "ir" ]]; then
  mkdir -p "$IR_DIR"
  rm -f "$IR_DIR"/*.ir
else
  mkdir -p "$AST_DIR"
  rm -f "$AST_DIR"/*.typed.ast
fi

pass=0
fail=0

for SRC in "${SOURCES[@]}"; do
  BASE="$(basename "${SRC%.min}")"
  printf '\n== %s ==\n' "$(basename "$SRC")"

  if [[ "$MODE" == "ir" ]]; then
    if "$EXE" "$SRC" -o "$IR_DIR/$BASE.ir"; then
      echo "IR generated: ir/$BASE.ir"
      pass=$((pass + 1))
    else
      echo "IR generation failed: $SRC" >&2
      rm -f "$IR_DIR/$BASE.ir"
      fail=$((fail + 1))
    fi
  else
    if "$EXE" --dump-typed-ast "$SRC" > "$AST_DIR/$BASE.typed.ast"; then
      echo "Typed AST generated: typed_ast/$BASE.typed.ast"
      pass=$((pass + 1))
    else
      echo "Typed AST generation failed: $SRC" >&2
      rm -f "$AST_DIR/$BASE.typed.ast"
      fail=$((fail + 1))
    fi
  fi
done

echo
echo "Successful: $pass/${#SOURCES[@]}"
echo "Failed: $fail"

if [[ "$fail" -eq 0 ]]; then
  exit 0
else
  exit 1
fi
