#!/usr/bin/env python3
"""Reject UI color constructors under src/app outside ui_theme.*."""

import pathlib
import re
import sys

# Keep this pattern identical to the T5b Q18 check.
PATTERN = (
    r"IM_COL32\(|ImColor\(|brand_color\(|"
    r"ImVec4\(\s*[0-9.]|ImVec4\{\s*[0-9.]|"
    r"glClearColor\(\s*[0-9.]"
)

SUFFIXES = {".h", ".hh", ".hpp", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1] / "src" / "app"
    pattern = re.compile(PATTERN)
    found = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in SUFFIXES or path.name.startswith("ui_theme."):
            continue
        for number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), 1
        ):
            if pattern.search(line):
                found.append(f"{path}:{number}:{line.strip()}")
    if found:
        print("color literals outside ui_theme.*:")
        print("\n".join(found))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
