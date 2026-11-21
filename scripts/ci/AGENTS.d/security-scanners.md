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
`requirements/locks/manifest.json`, skips only the scanner's own lock (other tool locks included), runs
`pip-audit --no-deps` per project lock, then applies
`pip-audit-ignore.json`. One exception per lock and advisory, with expiry; an
expired or unused entry fails. Every invocation first audits
`fixtures/pip-audit-known-vuln.txt` (`requests==2.19.0`) and refuses a green run
unless the expected requests version reports CVE-2018-18074 or
GHSA-x84v-xcm2-53pg. Empty reports fail. New lock -> manifest entry;
`python_audit` impact patterns must cover its inputs and output (contract test).
Pull requests route the audit; weekly schedules audit every lock.

Image: `docker-image.yml` installs the `TRIVY_VERSION` / `TRIVY_SHA256` binary,
builds `fixtures/trivy-negative.Dockerfile` (`ubuntu:18.04`) and requires
`trivy image --exit-code 3` to exit 3 before scanning the real `vmaf` image. `.trivyignore.yaml`
supplies dated misconfiguration and image-vulnerability exceptions; image edits
select the Docker gate. Trivy expires entries but does not reject unused ones. A changed
severity threshold or scanner list must keep the negative control failing.

Network: both gates need the advisory database. A Python gate analysis error exits 2; a Trivy error returns 1,
which the negative control refuses. Neither is a pass. Run the unit fixture with
`python3 -m unittest scripts/ci/tests/test_python_audit.py`.
