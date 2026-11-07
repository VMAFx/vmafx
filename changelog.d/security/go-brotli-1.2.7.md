- **`github.com/andybalholm/brotli` moves from v1.2.6 to v1.2.7** for GO-2026-6888
  (CVE-2026-111497, GHSA-c9gr-gmg4-2vqv). The two reports describe different denial-of-service
  paths: the Go vulnerability report names unbounded Huffman-table and block-type-tree
  allocation across many empty meta-blocks, and the upstream advisory names an invalid
  block-switch command that makes the decoder read past the end of its input, which in Go
  is a panic or an infinite loop. govulncheck found reachable call paths through
  `pkg/libvmaf` and `pkg/tune/executor`, so the pre-push security hook refused every push;
  with v1.2.7 it reports no vulnerability affecting the code.
