- **The silent-revert gate (ADR-1284) reported ordinary work as lost work.**
  `reverse-hunk` flagged every file a branch deliberately deletes — for a
  target commit that only *added* the file, all its lines are live, the merge
  removes them and puts nothing back, which is what the detector asks of a
  pure-addition commit — and the `resurrected` arm asked only whether a line
  was absent from the target, although it is defined as text the target had
  deleted coming back, so every line a merge commit authored while resolving a
  conflict matched. `reverse-hunk` now skips paths the merge deletes outright
  (`dropped` already tells a deletion the branch asked for from one it did
  not), and `resurrected` requires the line to be text the target once held
  and lost.
- **A reversal an accepted ADR mandates can now be declared in the tree.**
  `scripts/ci/silent-revert-allowlist.json` (ADR-1291) takes an entry naming
  the ADR, the detector, the commit being undone, the exact paths and a regex
  every line of the finding must match; a finding is downgraded to a notice
  only when all of those agree, so an undeclared finding in the same run still
  fails. The per-PR `revert:` / `reverts: #N` declaration is unchanged and
  stays the mechanism for a one-off revert.
