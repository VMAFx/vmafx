Restore ADR-0480's single owner for bootstrap collection-score names. Both
pooled and per-index score paths again consume `bootstrap_names.h`, with a fast
source-contract test preventing the shared header from becoming orphaned.
