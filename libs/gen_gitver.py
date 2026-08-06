#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# gen_gitver.py <out.h> [git_dir]
#
# Emit fw_gitver.h with FW_GIT_VERSION set to the 6-char short commit hash of
# HEAD (git rev-parse --short=6). Falls back to "000000" when git is missing or
# the command fails. The header is shared by the CAN/UDP firmware-upgrade libs to
# build the version string "v<M>.<m>.<p>_<6hex>". Invoked from the library
# CMakeLists at configure time.

import os
import subprocess
import sys


def main():
    if len(sys.argv) < 2:
        print("usage: gen_gitver.py <out.h> [git_dir]", file=sys.stderr)
        return 1
    out = sys.argv[1]
    git_dir = sys.argv[2] if len(sys.argv) > 2 else os.getcwd()

    sha = "000000"
    try:
        res = subprocess.run(
            ["git", "rev-parse", "--short=6", "HEAD"],
            cwd=git_dir,
            capture_output=True,
            text=True,
        )
        if res.returncode == 0:
            sha = res.stdout.strip() or "000000"
    except (OSError, FileNotFoundError):
        pass

    print('fw_gitver: FW_GIT_VERSION="%s"' % sha)

    lines = [
        "#ifndef FW_GITVER_H",
        "#define FW_GITVER_H",
        "",
        "/* 6-char git commit hash (injected at configure time by gen_gitver.py).",
        ' * Falls back to "000000" when git is missing or fails. Used by the',
        ' * CAN/UDP firmware-upgrade libs to build "v<M>.<m>.<p>_<6hex>". */',
        '#define FW_GIT_VERSION "%s"' % sha,
        "",
        "#endif /* FW_GITVER_H */",
    ]
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
