#!/usr/bin/env python3
"""validate_jsonl.py — line-by-line JSONL validity and structure check.

Usage: validate_jsonl.py <file.jsonl> [file2.jsonl ...]
Exit code 0 iff every line of every file parses as JSON (and, when
`--fields f1,f2` is given, contains the required fields).

Prints per-file: total lines, invalid lines (0 = pass), first bad line,
field summary for the first line.
"""

import argparse
import json
import sys


def validate(path, required_fields):
    total = 0
    bad = 0
    first_bad = None
    first_line = None
    with open(path) as f:
        for i, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            total += 1
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                bad += 1
                if first_bad is None:
                    first_bad = f"line {i}: {e}"
                continue
            if required_fields:
                missing = [k for k in required_fields if k not in obj]
                if missing:
                    bad += 1
                    if first_bad is None:
                        first_bad = f"line {i}: missing fields {missing}"
                    continue
            if first_line is None:
                first_line = obj
    return total, bad, first_bad, first_line


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--fields", default="",
                    help="comma-separated required fields")
    args = ap.parse_args()
    required = [f for f in args.fields.split(",") if f]
    ok = True
    for path in args.files:
        total, bad, first_bad, first_line = validate(path, required)
        status = "PASS" if bad == 0 else "FAIL"
        if bad:
            ok = False
        print(f"{status}  {path}: {total} lines, {bad} invalid"
              + (f" (first: {first_bad})" if first_bad else ""))
        if first_line is not None:
            keys = ",".join(sorted(first_line.keys()))
            print(f"      first-line fields: {keys}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
