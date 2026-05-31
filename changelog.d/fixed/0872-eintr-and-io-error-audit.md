- Audit and harden POSIX I/O return-value handling across fork-added C
  surfaces (ADR-0872). Two MCP transport drain loops
  (`transport_stdio.c`, `transport_uds.c`) that flush the rest of an
  over-length JSON-RPC request line now retry on `EINTR` instead of
  treating the spurious signal-interruption as end-of-stream — under
  signal pressure the previous loop could exit early and leave half a
  line in the kernel buffer, corrupting the next request boundary.
  Seven `close(2)` call sites whose return values were silently
  discarded (`core/src/libvmaf.c`, `core/src/feature/cambi.c`,
  `core/src/sycl/dmabuf_import.cpp` ×2, `core/tools/vmaf_vpl.c` ×3)
  now carry an explicit `(void)` cast in line with CLAUDE.md §6 and
  Power-of-10 rule 7 ("every non-void return value is checked or
  `(void)`-discarded"). No behavioural change for the `close()` paths
  (they were already on fail / cleanup branches where rc could not be
  acted on); the cast makes the intent visible to clang-tidy and to
  the next maintainer.
