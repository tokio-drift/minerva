#!/usr/bin/env python3
"""
Runs `make conflicts` and prints a condensed summary:
count of shift/reduce and reduce/reduce conflicts, grouped by token,
with one example each instead of the full derivation trees.

Usage: python3 scripts/summarize_conflicts.py
"""
import re
import subprocess
import sys
from collections import defaultdict

CONFLICT_RE = re.compile(
    r"warning: (shift/reduce|reduce/reduce) conflict on token (\S+)"
)
EXAMPLE_RE = re.compile(r"^\s*(?:First example|Example):\s*(.+)$")


def main():
    try:
        result = subprocess.run(
            ["make", "conflicts"],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except FileNotFoundError:
        sys.exit("error: could not find 'make'. Run this from the repo root.")
    except subprocess.TimeoutExpired:
        sys.exit("error: 'make conflicts' took too long (>120s)")

    output = result.stdout + result.stderr
    lines = output.splitlines()

    conflicts = []
    current = None

    for line in lines:
        m = CONFLICT_RE.search(line)
        if m:
            if current:
                conflicts.append(current)
            current = {"kind": m.group(1), "token": m.group(2), "example": None}
            continue
        if current and current["example"] is None:
            em = EXAMPLE_RE.match(line)
            if em:
                current["example"] = em.group(1).strip()
    if current:
        conflicts.append(current)

    sr = [c for c in conflicts if c["kind"] == "shift/reduce"]
    rr = [c for c in conflicts if c["kind"] == "reduce/reduce"]

    by_token = defaultdict(list)
    for c in conflicts:
        by_token[c["token"]].append(c)

    print("=" * 60)
    print("BISON CONFLICT SUMMARY")
    print("=" * 60)
    print(f"Total conflicts:     {len(conflicts)}")
    print(f"  Shift/reduce:      {len(sr)}")
    print(f"  Reduce/reduce:     {len(rr)}")
    print()

    if not conflicts:
        print("No conflicts found.")
        return

    print("By token:")
    for token, items in sorted(by_token.items(), key=lambda kv: -len(kv[1])):
        print(f"  {token:>6}  x{len(items)}")
    print()

    print("-" * 60)
    print("EXAMPLES (one per token)")
    print("-" * 60)
    seen_tokens = set()
    for c in conflicts:
        if c["token"] in seen_tokens:
            continue
        seen_tokens.add(c["token"])
        print(f"\n[{c['kind']}] token {c['token']}  ({len(by_token[c['token']])} occurrence(s))")
        if c["example"]:
            print(f"  {c['example']}")
        else:
            print("  (no example captured)")

    if "time limit exceeded" in output:
        n = output.count("time limit exceeded")
        print(f"\nNote: bison hit its counterexample search time limit {n} time(s);")
        print("those conflicts still got a (non-minimal) example above.")


if __name__ == "__main__":
    main()