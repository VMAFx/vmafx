- CI: `ci-escalate.yml` cancels every running workflow run of the pull request
  first and then awaits them together within one 180-second bound, instead of
  cancelling and awaiting each run in turn. With several running workflows the
  serial wait passed the job's 15-minute limit, and the job was cancelled before
  it re-ran anything (`scripts/ci/ci_escalate.py`).
