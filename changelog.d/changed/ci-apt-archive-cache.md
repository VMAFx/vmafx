- **The `MCP Smoke` and `Tiny AI` jobs now cache apt package archives (.deb files).**
  Package lists are declared per job in `.github/apt/mcp-smoke.txt` and
  `.github/apt/tiny-ai.txt`, and restored archives are copied into
  `/var/cache/apt/archives` before install so slow package mirrors no longer
  eat into the apt step's 20-minute timeout (`.github/workflows/tests-and-quality-gates.yml`).
