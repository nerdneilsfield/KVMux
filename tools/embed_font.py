#!/usr/bin/env python3
"""Write a C++ translation unit that embeds one font file.

CMake runs this from the vendored file. It does not download anything.
"""

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: embed_font.py SOURCE OUTPUT", file=sys.stderr)
        return 2
    data = Path(sys.argv[1]).read_bytes()
    lines = [
        "// Generated from third_party/fonts/Inter-Regular.ttf. Do not edit.",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace kvmux::font {",
        "extern const std::uint8_t kInterRegular[] = {",
    ]
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append("    " + ", ".join(str(byte) for byte in chunk) + ",")
    lines.extend(
        [
            "};",
            "extern const std::size_t kInterRegularSize = sizeof(kInterRegular);",
            "}  // namespace kvmux::font",
            "",
        ]
    )
    Path(sys.argv[2]).write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
