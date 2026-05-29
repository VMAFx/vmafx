# C++23 Wave 7 — activate `cpu.cpp` (drop orphan `cpu.c`)

`core/src/cpu.c` was an orphan left over from Wave 5 (ADR-0735), which had created
`cpu.cpp` with thread-safe `std::atomic` flag initialisation but never deleted the old
`.c`. Meson continued to compile the `.c`. This PR removes `cpu.c` and updates
`meson.build` to compile `cpu.cpp`, making the Wave 5 improvement effective.

ADR: [0755](../docs/adr/0755-cpp23-wave7-single-file.md)
