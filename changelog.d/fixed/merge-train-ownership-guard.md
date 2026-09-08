- Guard local merge-train promotion, rebase, and merge actions against stacked
  bases, holds, release PRs, and active source owners. Rebase failures retain
  evidence and prevent promotion; merges require executed full local gate
  receipts and present, passing required checks. Runtime migration is explicit.
