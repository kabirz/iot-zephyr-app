#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# sync_skills.py [--check]
#
# Mirror every skill under .agents/skills/ into .claude/skills/ so the same
# skill file is discovered by Claude Code (.claude/skills/), OpenCode and ZCode
# (both read .agents/skills/). The .agents/skills/ tree is the canonical source.
#
# Default mode  : copy/overwrite the .claude/skills/ mirror from .agents/skills/.
# --check mode  : compare only; exit 0 if in sync, exit 1 (and print a diff) if
#                 any file is missing or differs. Intended for pre-commit / CI.

import argparse
import filecmp
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC = REPO_ROOT / ".agents" / "skills"
DST = REPO_ROOT / ".claude" / "skills"


def _iter_skill_files():
    """Yield every regular file under SRC, as paths relative to SRC."""
    if not SRC.is_dir():
        return
    for p in SRC.rglob("*"):
        if p.is_file():
            yield p.relative_to(SRC)


def run(check: bool) -> int:
    if not SRC.is_dir():
        print(f"sync_skills: source dir not found: {SRC}", file=sys.stderr)
        return 1

    src_files = list(_iter_skill_files())
    if not src_files:
        print("sync_skills: no skills found under .agents/skills/", file=sys.stderr)
        return 1

    mismatches = []

    for rel in src_files:
        src_file = SRC / rel
        dst_file = DST / rel

        if not dst_file.exists():
            mismatches.append(str(rel))
            if not check:
                dst_file.parent.mkdir(parents=True, exist_ok=True)
                dst_file.write_bytes(src_file.read_bytes())
                print(f"sync_skills: created {dst_file.relative_to(REPO_ROOT)}")
            continue

        if not filecmp.cmp(src_file, dst_file, shallow=False):
            mismatches.append(str(rel))
            if not check:
                dst_file.parent.mkdir(parents=True, exist_ok=True)
                dst_file.write_bytes(src_file.read_bytes())
                print(f"sync_skills: updated {dst_file.relative_to(REPO_ROOT)}")

    # Report stale files in the mirror that no longer exist in the source.
    if DST.is_dir():
        for p in DST.rglob("*"):
            if p.is_file():
                rel = p.relative_to(DST)
                if not (SRC / rel).exists():
                    mismatches.append(f"(stale) {rel}")
                    if not check:
                        p.unlink()
                        print(f"sync_skills: removed stale {p.relative_to(REPO_ROOT)}")

    if check:
        if mismatches:
            print("sync_skills: out of sync:", file=sys.stderr)
            for m in mismatches:
                print(f"  - {m}", file=sys.stderr)
            print("  Run: python scripts/sync_skills.py", file=sys.stderr)
            return 1
        print("sync_skills: in sync")
        return 0

    if mismatches:
        print(f"sync_skills: synced {len(mismatches)} file(s)")
    else:
        print("sync_skills: already in sync")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Mirror .agents/skills/ into .claude/skills/ for cross-tool discovery.",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="only verify the mirror is in sync; exit non-zero if not",
    )
    args = parser.parse_args()
    return run(check=args.check)


if __name__ == "__main__":
    sys.exit(main())
