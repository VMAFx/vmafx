- CI: the Required Checks Aggregator waits out GitHub API 5xx answers for 15
  minutes. A longer outage fails the job with `Transient GitHub API failure:`
  instead of an unhandled `HttpError`, so it reads as a rerun and not as a code
  failure.
