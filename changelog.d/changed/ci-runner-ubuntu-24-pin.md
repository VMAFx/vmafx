**CI**: pin all non-Docker runner jobs from `ubuntu-latest` to `ubuntu-24.04`
to prevent silent toolchain drift when GitHub promotes the floating alias to
Ubuntu 26.04 (expected H2 2026). CUDA pins (13.2.0), Windows runners
(windows-2025), and macOS runners (macos-latest) are unchanged. Container
images remain at their existing pins. ADR-0802.
