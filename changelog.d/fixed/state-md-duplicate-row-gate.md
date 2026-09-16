- **The `docs/state.md` row-hygiene gate reported a file with 33
  duplicated rows as clean.** ADR-0165 says every bug id appears
  exactly once, and `scripts/ci/check-state-md-rows.sh` existed to
  enforce it — but it only matched ids that were **bold** and began
  with `T-`. That hid 95 of 576 id-bearing rows (17%), including a
  byte-identical duplicate of `T-CUDA-MUL24-AUDIT-2026-05-28`; it hid
  all 34 `Netflix#NNN` rows, which had accumulated 13 duplicate pairs;
  and it hid the `**T6-1**` / `**T7-16**` tranche ids, which had four
  more. Roughly 143 rows open with prose and carry no id at all, so a
  further 13 duplicates were unreachable by any id-based check. The
  gate now matches all four id shapes and adds a second,
  shape-independent check for a verbatim repeated row — normalising
  away the `_(verified YYYY-MM-DD: ...)_` annotations a later sweep
  appends to one copy, which is what defeated a naive comparison.
  Column headers, which repeat once per section by design, are
  excluded. Detection and reporting now share one extraction rule; the
  previous split meant every non-bold duplicate printed "appears on
  lines:" followed by nothing.
  The 33 duplicate rows are resolved. The two copies were **not**
  interchangeable and the newer one was **not** always the later line:
  most pairs kept the later copy (it carries the verification
  annotation, and in two cases resolves "this PR" to the real PR
  number), but `T6-2` kept the earlier copy, which says PR #469 is
  merged where the later says it is still in flight, and
  `Netflix/vmaf#1494` kept the earlier copy, which records ADR-1191 as
  closed where the later does not. Every pair was read before either
  copy was deleted, and each deletion was verified to leave its twin
  in place. Five new gate tests cover the shapes that were invisible,
  and one covers the header false positive.
