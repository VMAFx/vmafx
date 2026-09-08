- Guard local merge-train promotion, rebase, and merge actions against stacked
  bases, holds, release PRs, and active source owners. Rebase failures retain
  evidence and prevent promotion; merges require executed full local gate
  receipts and present, passing required checks. A previewed migration helper
  preserves originals/holds and installs paused adapters with hash checks and
  read-only observer defaults; process restart remains explicit.
