- **The praetor pin moves to `019b879ec691`.** `praetorctl adopt` adds the hash-pinned REUSE
  requirements, the secrets gate folder, the figures ESLint entry and licence sidecars for
  `.mcp.json` and `opencode.json`. The Gitleaks history scan reads the diff of merge commits
  (`--log-opts="-m --full-history HEAD"`), and 21 fixtures that tests pin by bytes keep their
  checked-in bytes on every platform (`-text` in `.gitattributes`).
