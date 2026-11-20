---
paths:
  - scripts/ci/check_python_audit.py
  - scripts/ci/tests/test_python_audit.py
  - scripts/ci/pip-audit-ignore.json
  - scripts/ci/fixtures/pip-audit-known-vuln.txt
  - scripts/ci/fixtures/trivy-negative.Dockerfile
  - .github/workflows/security-scans.yml
  - .github/workflows/docker-image.yml
  - .trivyignore.yaml
invariant: Python locks audited with a planted advisory; Trivy image gate rejects a planted EOL base; dated exceptions cannot rot.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Dependency audit and image scan

Python: `check_python_audit.py` reads the lock list from
`requirements/locks/manifest.json`, skips the scanner's own lock, runs
`pip-audit --no-deps` per project lock, then applies
`pip-audit-ignore.json`. One exception per lock and advisory, with expiry; an
expired or unused entry fails. `--self-test` audits
`fixtures/pip-audit-known-vuln.txt` (`requests==2.19.0`) and refuses a green run
unless CVE-2018-18074 appears. New project lock -> manifest entry; no second
list.

Image: `docker-image.yml` installs the `TRIVY_VERSION` / `TRIVY_SHA256` binary,
builds `fixtures/trivy-negative.Dockerfile` (`ubuntu:18.04`) and requires
`trivy image` to exit 1 before scanning the real `vmaf` image. `.trivyignore.yaml`
supplies dated misconfiguration and image-vulnerability exceptions. A changed
severity threshold or scanner list must keep the negative control failing.

Network: both gates need the advisory database. A scanner error exits 2; it is
never a pass. Run the unit fixture with
`python3 -m unittest scripts/ci/tests/test_python_audit.py`.
