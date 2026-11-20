- **CI:** wait on API rate limits and poll less often (issue 3582). Refresh the
  final pending sample after the last polling sleep, and keep mixed rate-limit
  and server-error retries within the script's time budget.
