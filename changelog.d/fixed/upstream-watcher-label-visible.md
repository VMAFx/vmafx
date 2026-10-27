- **The upstream-watcher workflow waits until a new label is usable.**
  A label created moments before could still be unknown to `gh issue create`, which
  failed the 2026-10-27 scheduled run. `scripts/upstream-watcher/ensure_label.sh` now
  reads the label list after creating the label, up to 10 times, and fails naming the
  label when it never appears.
