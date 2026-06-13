- chore(python): `__init__.py` export-completeness audit across fork-added
  Python packages. Adds `__all__` to eight package surfaces
  (`ai`, `ai.data`, `ai.train`, `ai.src.vmaf_train`,
  `ai.src.vmaf_train.data`, `dev_llm.vmaf_dev_llm`, `mcp_server.vmaf_mcp`,
  `scripts.lib`), backfills missing Lusoris + SPDX headers on three of
  them, and refreshes the stale `ai/train/__init__.py` docstring (3 of 6
  sub-modules were listed; now all 6). Upstream-mirror packages
  (`compat/python-vmaf/**`, `python/test/`) and pure test-marker stubs are
  out of scope. No runtime-behaviour change — `from <pkg> import *` had
  zero callers in tree against any touched package. Codifies the pattern
  in [ADR-0911](../docs/adr/0911-init-py-export-completeness-audit.md).
