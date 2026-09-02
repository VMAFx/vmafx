Rename source directories as part of the VMAFX rebrand (ADR-0700):
`libvmaf/` → `core/` (C library and build root) and
`python/vmaf/` → `compat/python-vmaf/` (Python harness package).
Output artifacts (`libvmaf.so`, `libvmaf.pc`, `<libvmaf/...>` install
headers, public C symbol names) are unchanged — this is a source-tree
layout change only. A compatibility shim at `python/vmaf/__init__.py`
and a `compat/vmaf` symlink preserve `import vmaf` for existing scripts.
