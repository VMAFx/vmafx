- **MCP HTTP aiohttp security floor** — raise the optional `[http]` extra from
  `aiohttp>=3.9.0` to `aiohttp>=3.14.3`, excluding every release affected by
  GHSA-cq5v-8q36-5273 / CVE-2026-69244. The HTTP transport does not register
  static resources or enable `follow_symlinks`; its documented direct-install
  command now quotes both version specifiers so POSIX shells do not interpret
  `>` as output redirection.
