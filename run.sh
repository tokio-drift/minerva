#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE=run
EXE="${MINERVA_BIN:-$ROOT/minerva}"
BUILD_DEFAULT=1
if [[ -n "${MINERVA_BIN:-}" ]]; then BUILD_DEFAULT=0; fi

usage() {
  echo "Usage: $0 [BINARY] [run|ir|ast|mips|test|clean]" >&2
  exit 2
}

if (( $# )); then
  case "$1" in
    run|ir|ast|mips|test|clean) MODE="$1"; shift ;;
    *) EXE="$1"; BUILD_DEFAULT=0; shift
       if (( $# )); then MODE="$1"; shift; fi ;;
  esac
fi
(( $# == 0 )) || usage
case "$MODE" in run|ir|ast|mips|test|clean) ;; *) usage ;; esac
if [[ "$MODE" == test ]]; then MODE=run; fi

if [[ "$MODE" == clean ]]; then
  rm -rf "$ROOT/ir" "$ROOT/typed_ast" "$ROOT/mips" "$ROOT/generated"
  echo "Removed generated IR, typed AST, MIPS and demo file folders."
  exit 0
fi

if [[ "$EXE" != /* ]]; then EXE="$PWD/$EXE"; fi
if [[ ! -f "$EXE" || ! -x "$EXE" ]]; then
  if (( BUILD_DEFAULT )); then make -C "$ROOT" || exit 2
  else echo "Compiler executable not found: $EXE" >&2; exit 2
  fi
fi

case "$MODE" in
  ir) STAGE=--ir; OUTPUT="$ROOT/ir"; SUFFIX=.ir ;;
  ast) STAGE=--typed-ast; OUTPUT="$ROOT/typed_ast"; SUFFIX=.typed.ast ;;
  mips) STAGE=--mips; OUTPUT="$ROOT/mips"; SUFFIX=.s ;;
esac

mapfile -d '' -t SOURCES < <(find "$ROOT/test" -maxdepth 1 -type f -name '*.min' -print0 | sort -zV)
(( ${#SOURCES[@]} )) || { echo "No source demos found under test/." >&2; exit 2; }

run_demo() {
  if [[ "$(basename "$1")" == test8_io_and_arguments.min ]]; then
    mkdir -p "$ROOT/generated"
    echo 'Sample input: 12 3.5 X minerva; arguments: hello "compiler demo" generated/file_demo.txt'
    printf '12 3.5 X minerva\n' | "$EXE" --run "$1" -- hello "compiler demo" "$ROOT/generated/file_demo.txt"
  else
    "$EXE" --run "$1"
  fi
}

pass=0
fail=0
for source in "${SOURCES[@]}"; do
  relative="${source#"$ROOT/test/"}"
  printf '\n== %s ==\n' "$relative"
  if [[ "$MODE" == run ]]; then
    if run_demo "$source"; then pass=$((pass + 1))
    else fail=$((fail + 1)); echo "FAIL $relative" >&2
    fi
  else
    output="$OUTPUT/$relative$SUFFIX"
    mkdir -p "$(dirname "$output")"
    rm -f "$output"
    if "$EXE" "$STAGE" "$source" -o "$output"; then
      echo "PASS $relative"; pass=$((pass + 1))
    else
      echo "FAIL $relative" >&2; rm -f "$output"; fail=$((fail + 1))
    fi
  fi
done
printf '\nTotal: %d, passed: %d, failed: %d\n' "${#SOURCES[@]}" "$pass" "$fail"
[[ "$fail" -eq 0 ]]
