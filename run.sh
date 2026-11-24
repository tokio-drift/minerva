#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE=ir
EXE="${MINERVA_BIN:-$ROOT/minerva}"
BUILD_DEFAULT=1
if [[ -n "${MINERVA_BIN:-}" ]]; then BUILD_DEFAULT=0; fi

usage() {
  echo "Usage: $0 [BINARY] [ir|ast|mips|test|clean]" >&2
  exit 2
}

if (( $# )); then
  case "$1" in
    ir|ast|mips|test|clean) MODE="$1"; shift ;;
    *) EXE="$1"; BUILD_DEFAULT=0; shift
       if (( $# )); then MODE="$1"; shift; fi ;;
  esac
fi
(( $# == 0 )) || usage
case "$MODE" in ir|ast|mips|test|clean) ;; *) usage ;; esac

if [[ "$MODE" == clean ]]; then
  rm -rf "$ROOT/ir" "$ROOT/typed_ast" "$ROOT/mips"
  echo "Removed generated IR, typed AST and MIPS folders."
  exit 0
fi

if [[ "$EXE" != /* ]]; then EXE="$PWD/$EXE"; fi
if [[ ! -f "$EXE" || ! -x "$EXE" ]]; then
  if (( BUILD_DEFAULT )); then
    make -C "$ROOT" || exit 2
  else
    echo "Compiler executable not found: $EXE" >&2
    exit 2
  fi
fi

if [[ "$MODE" == test ]]; then
  exec python3 "$ROOT/test/run_tests.py" --compiler "$EXE"
fi

case "$MODE" in
  ir) STAGE=--ir; OUTPUT="$ROOT/ir"; SUFFIX=.ir ;;
  ast) STAGE=--typed-ast; OUTPUT="$ROOT/typed_ast"; SUFFIX=.typed.ast ;;
  mips) STAGE=--mips; OUTPUT="$ROOT/mips"; SUFFIX=.s ;;
esac

mapfile -d '' -t SOURCES < <(
  find "$ROOT/test" -type f ! -path "$ROOT/test/negative/*" ! -path "$ROOT/test/ir/*" \
    \( -name '*.min' -o -name '*.c' -o -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.C' \) \
    -print0 | sort -z
)
(( ${#SOURCES[@]} )) || { echo "No source demos found under test/." >&2; exit 2; }

pass=0
fail=0
for source in "${SOURCES[@]}"; do
  relative="${source#"$ROOT/test/"}"
  output="$OUTPUT/$relative$SUFFIX"
  mkdir -p "$(dirname "$output")"
  rm -f "$output"
  if "$EXE" "$STAGE" "$source" -o "$output"; then
    echo "PASS $relative"
    pass=$((pass + 1))
  else
    echo "FAIL $relative" >&2
    rm -f "$output"
    fail=$((fail + 1))
  fi
done
printf 'Total: %d, passed: %d, failed: %d\n' "${#SOURCES[@]}" "$pass" "$fail"
[[ "$fail" -eq 0 ]]
