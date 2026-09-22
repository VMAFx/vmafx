- Separate machine-local data by lifecycle: agent state and bounded cache use
  `.workingdir/`, datasets and reusable derived data use `.corpus/`, and public
  evidence links only to tracked artifacts. Retire active references to the
  former numbered workspace and add a CI contract preventing its return.
- Make the agent-dispatch eligibility check and hardware-corpus producer fail
  closed instead of reporting incomplete GitHub/task/corpus checks as success.
