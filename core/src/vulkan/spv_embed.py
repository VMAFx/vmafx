#!/usr/bin/env python3
"""Embed a SPIR-V binary as a C array header.

Usage: spv_embed.py <input.spv> <output.h> <symbol_name>

Generates:
    static const unsigned int <symbol_name>_spv[] = { 0x..., ... };
    static const unsigned int <symbol_name>_spv_size = <byte_count>;

Used by libvmaf/src/vulkan/meson.build's glslc custom_target chain
to splice the compiled compute shaders into the host-side dispatch
TUs in libvmaf/src/feature/vulkan/. SPIR-V is little-endian uint32,
so we read the .spv as 4-byte words and emit one 0x%08x per word.
"""

import sys


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: spv_embed.py <input.spv> <output.h> <symbol_name>", file=sys.stderr)
        return 1
    in_path, out_path, name = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(in_path, "rb") as fh:
        data = fh.read()
    if len(data) % 4 != 0:
        print(f"spv_embed: {in_path} not a multiple of 4 bytes", file=sys.stderr)
        return 2
    words = [int.from_bytes(data[i : i + 4], "little") for i in range(0, len(data), 4)]
    with open(out_path, "w") as f:
        f.write(f"/* Auto-generated from {name}.comp by libvmaf/src/vulkan/spv_embed.py */\n")
        f.write(f"static const unsigned int {name}_spv[] = {{\n  ")
        f.write(", ".join(f"0x{w:08x}" for w in words))
        f.write("\n};\n")
        f.write(f"static const unsigned int {name}_spv_size = {len(words) * 4};\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
