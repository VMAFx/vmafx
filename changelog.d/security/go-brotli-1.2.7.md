- **`github.com/andybalholm/brotli` moves from v1.2.6 to v1.2.7** for GO-2026-6888 (excessive
  memory allocation when decompressing crafted streams). govulncheck found reachable call paths
  through `pkg/libvmaf` and `pkg/tune/executor`, so the pre-push security hook refused every push;
  with v1.2.7 it reports no vulnerability affecting the code.
