- **An empty PR body was reported as six missing deliverables.** Both
  `scripts/ci/deliverables-check.sh` and
  `scripts/ci/validate-pr-body.sh` selected their input with
  `[ ! -t 0 ]`, which is true in *any* non-interactive shell — a CI
  step, a git hook, `bash script.sh </dev/null` — whether or not
  anything is actually piped. The stdin branch was therefore taken with
  nothing to read, `cat` yielded an empty string, and the ADR-0108
  parser dutifully reported all six deliverables missing. That is a
  true statement about an empty string and a misleading one about the
  PR: it sends the author hunting for a checklist-syntax bug when the
  real fault is that no body ever arrived. Both scripts now detect a
  blank body and say which it is — "nothing arrived on stdin" (usage
  error, exit 2) when the producer is at fault, and "the PR description
  is empty" (exit 1) when a body was supplied and is genuinely blank.
  `validate-pr-body.sh` also honours `$PR_BODY`, so it and
  `make pr-check` — two entry points to the same parser — take the same
  input the same way; previously setting `PR_BODY` and running the
  validator silently read empty stdin instead. Five new cases in
  `scripts/ci/test-validate-pr-body.sh` (13 total).
