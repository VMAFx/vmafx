- **System-wide Python installs no longer fail on a distribution-installed package.** Since the
  build lock pins PyYAML 6.0.3, `sudo pip3 install --break-system-packages` stopped on Ubuntu's
  `python3-yaml` 6.0.1 (`Cannot uninstall PyYAML 6.0.1, RECORD file not found`), which turned the
  Netflix golden, sanitizer, coverage and CodeQL jobs red. Every such install in the workflows,
  `docker/Dockerfile.production-gpu` and `mcp-server/vmaf-mcp/Dockerfile` passes
  `--ignore-installed`, and `scripts/ci/check_python_dependency_locks.py` refuses an install that
  breaks system packages without it (`--user` installs are exempt).
