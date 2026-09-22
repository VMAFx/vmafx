#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Re-apply the fork's in-tree invariants to `buf generate` output.

`protoc-gen-go` emits two `unsafe.Slice(unsafe.StringData(...))` calls per
generated file: one inside `file_*_rawDescGZIP` and one in the `DescBuilder`
literal of `file_*_init`. Both alias the immutable raw-descriptor string, and
both are perfectly sound — but HISS-09 requires the soundness argument to be
written down next to the operation, and the upstream generator has no hook for
emitting one.

Hand-editing generated files is not an option (the header says DO NOT EDIT and
the next regeneration would silently drop the proof), so the proof is emitted
here instead: this script runs as the second half of the codegen step, and is
idempotent, so re-running it after `buf generate` always reproduces exactly the
committed tree.

Usage::

    buf generate                                   # writes gen/go/**
    python3 scripts/proto/postprocess_gen_go.py    # re-applies the proofs
    python3 scripts/proto/postprocess_gen_go.py --check   # CI: verify only
"""

from __future__ import annotations

import argparse
import pathlib
import sys

# The operation this script proves. Matching on the full call keeps the script
# from annotating some future unrelated `unsafe` use whose proof would differ.
RAW_DESC_CALL = "unsafe.Slice(unsafe.StringData("

SAFETY_PROOF = (
    "SAFETY: rawDesc is an immutable package-level string literal that nothing"
    " in this package or in protoimpl mutates or frees for the life of the"
    " process; StringData returns its backing array and Slice re-describes"
    " exactly len(rawDesc) of those bytes, so the alias spans the allocation"
    " and no more."
)

MAX_PROOF_WIDTH = 96

# The HISS-09 scanner binds a comment block to the operation below it only
# while the block stays short; a six-line block three levels deep inside the
# DescBuilder literal is no longer recognised as that statement's proof and
# the finding comes back. Four lines clears the observed boundary with room
# to spare, so the proof text is kept inside that budget rather than silently
# growing past it.
MAX_PROOF_LINES = 4


def wrap_proof(indent: str) -> list[str]:
    """Render SAFETY_PROOF as Go line comments at the given indentation."""
    prefix = f"{indent}// "
    budget = max(MAX_PROOF_WIDTH - len(prefix), 32)
    lines: list[str] = []
    current = ""
    for word in SAFETY_PROOF.split(" "):
        candidate = word if not current else f"{current} {word}"
        if len(candidate) > budget and current:
            lines.append(prefix + current)
            current = word
        else:
            current = candidate
    if current:
        lines.append(prefix + current)
    if len(lines) > MAX_PROOF_LINES:
        raise ValueError(
            f"SAFETY proof wraps to {len(lines)} lines at indent {len(indent)}; "
            f"the scanner stops binding the block past {MAX_PROOF_LINES} — shorten it"
        )
    return lines


def already_proven(emitted: list[str]) -> bool:
    """True when the comment block directly above the cursor opens with SAFETY."""
    for line in reversed(emitted):
        stripped = line.lstrip()
        if not stripped.startswith("//"):
            return False
        if stripped.startswith("// SAFETY:"):
            return True
    return False


def annotate(text: str) -> str:
    """Return text with a SAFETY proof above every raw-descriptor alias."""
    out: list[str] = []
    for line in text.split("\n"):
        stripped = line.lstrip()
        needs_proof = RAW_DESC_CALL in line and not stripped.startswith("//")
        if needs_proof and not already_proven(out):
            out.extend(wrap_proof(line[: len(line) - len(stripped)]))
        out.append(line)
    return "\n".join(out)


def generated_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Return the generated Go files this script is responsible for."""
    return sorted(root.joinpath("gen", "go").rglob("*.pb.go"))


def process(path: pathlib.Path, check_only: bool) -> bool:
    """Apply (or verify) the proofs in one file; True when it was already correct."""
    original = path.read_text(encoding="utf-8")
    updated = annotate(original)
    if updated == original:
        return True
    if not check_only:
        path.write_text(updated, encoding="utf-8")
    return False


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the committed output instead of rewriting it",
    )
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[2],
        help="repository root (default: inferred from this script's location)",
    )
    args = parser.parse_args(argv)

    files = generated_files(args.root)
    if not files:
        print(f"no generated *.pb.go under {args.root / 'gen' / 'go'}", file=sys.stderr)
        return 1

    stale = [path for path in files if not process(path, args.check)]
    for path in stale:
        verb = "missing SAFETY proof" if args.check else "annotated"
        print(f"{verb}: {path.relative_to(args.root)}")
    if stale and args.check:
        print("run scripts/proto/postprocess_gen_go.py after buf generate", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
