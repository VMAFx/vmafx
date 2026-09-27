- Release candidates can publish `vmaf-mcp`. The supply-chain workflow looked
  for `vmaf_mcp-1.0.0-rc.1` wheels, but Python tools name them
  `vmaf_mcp-1.0.0rc1`, so the MCP build found nothing and SBOM, signing, PyPI
  upload and release attachment were all skipped. It now matches the PEP 440
  spelling.
