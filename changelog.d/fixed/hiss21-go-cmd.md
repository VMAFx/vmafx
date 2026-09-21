- HISS-21 burn-down across `cmd/**` (vmafx-tune, vmafx-node, vmafx-mcp,
  vmafx-controller, vmafx-server, vmafx-operator): 93 invariant violations
  discharged with structural fixes, no suppressions and no baseline edits.
  Unbounded `for {}` loops now state a real exit condition or a justified
  finite bound — the daemon loops run while their context is live, the gRPC
  stream reads are bounded by the limit they already enforced and report a
  peer that streams past it, the repo-root walks are bounded by the starting
  path's depth, and the CAS retry by the peak it is publishing. Functions over
  the 60-line bound are split along seams the code already had, leaving error
  strings, check order, argv order, SQL text, JSON key order and resource
  unwind order unchanged. Discarded errors are handled rather than blanked:
  the bpf2go stub's `Close` now returns an error like the real generated
  bindings, `MarkFlagRequired` goes through the package's existing exit-2
  helper, and the remaining close, shutdown, env-bridge and short-write
  failures are reported instead of dropped. `unsafe.Sizeof` and
  `unsafe.Pointer` in the eBPF ring-buffer decode carry `// SAFETY:` proofs
  naming the bound that makes each sound, and the MCP schema marshaller no
  longer panics — a schema that cannot marshal registers the permissive empty
  object and logs, rather than taking every other tool down with it.
