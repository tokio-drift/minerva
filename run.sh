
#!/usr/bin/env bash
# Usage: ./run.sh [ir|ast|clean]
# Default: generate IR for every supported source file under test/.

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
    echo "Usage: $0 [ir|ast|clean]" >&2
    exit 2
    ;;
esac

if [[ "$MODE" == "clean" ]]; then
  rm -rf "$IR_DIR" "$AST_DIR"
  echo "Removed generated IR and typed AST folders."
  exit 0
fi

if [[ ! -x "$EXE" ]]; then
  echo "Compiler executable missing; building compiler..."
  if ! make -C "$ROOT"; then
    echo "Build failed." >&2
    exit 2
  fi
fi

if [[ ! -d "$TEST_DIR" ]]; then
  echo "Test directory not found: $TEST_DIR" >&2
  exit 2
fi

# Find any number of supported files, including files in subdirectories.
# No fixed test-count limit.
mapfile -d '' -t SOURCES < <(
  find "$TEST_DIR" -type f \( \
    -name '*.min' -o \
    -name '*.c' -o \
    -name '*.cpp' -o \
    -name '*.cc' -o \
    -name '*.cxx' -o \
    -name '*.C' \
  \) -print0 | sort -z
)

if [[ ${#SOURCES[@]} -eq 0 ]]; then
  echo "No supported source files found under $TEST_DIR." >&2
  echo "Supported extensions: .min, .c, .cpp, .cc, .cxx, .C" >&2
  exit 2
fi

if [[ "$MODE" == "ir" ]]; then
  mkdir -p "$IR_DIR"
else
  mkdir -p "$AST_DIR"
fi

pass=0
fail=0

for SRC in "${SOURCES[@]}"; do
  REL="${SRC#"$TEST_DIR"/}"

  # Preserve the original extension and subdirectory structure to prevent
  # collisions between files such as example.min and example.cpp.
  if [[ "$MODE" == "ir" ]]; then
    OUT="$IR_DIR/$REL.ir"
  else
    OUT="$AST_DIR/$REL.typed.ast"
  fi

  mkdir -p "$(dirname "$OUT")"
  rm -f "$OUT"

  printf '\n== %s ==\n' "$REL"

  if [[ "$MODE" == "ir" ]]; then
    if "$EXE" "$SRC" -o "$OUT"; then
      echo "IR generated: ${OUT#"$ROOT"/}"
      pass=$((pass + 1))
    else
      echo "IR generation failed: $REL" >&2
      rm -f "$OUT"
      fail=$((fail + 1))
    fi
  else
    if "$EXE" --dump-typed-ast "$SRC" > "$OUT"; then
      echo "Typed AST generated: ${OUT#"$ROOT"/}"
      pass=$((pass + 1))
    else
      echo "Typed AST generation failed: $REL" >&2
      rm -f "$OUT"
      fail=$((fail + 1))
    fi
  fi
done

echo
echo "Total: ${#SOURCES[@]}"
echo "Passed: $pass"
echo "Failed: $fail"

[[ "$fail" -eq 0 ]]
