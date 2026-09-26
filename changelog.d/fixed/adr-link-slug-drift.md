- **Every `adr/NNNN-slug.md` link under `docs/` now resolves, and a gate keeps it
  that way.** An ADR link carries the decision's identity twice, as a number and
  as a slug, and either half can rot alone: 98 were broken, 35 because an ADR
  collision sweep (lusoris/vmaf#310, lusoris/vmaf#752) renumbered the file while
  the slug stayed
  right, and 63 because the ADR was renamed while the number stayed right. Each is
  repaired from whichever half still identifies it — slug first, which also
  rewrites the `[ADR-NNNN]` text, since a renumbered citation is wrong in both
  halves. `scripts/ci/check-adr-links.py` runs as a pre-commit hook on any `docs/`
  change and refuses to guess when neither half resolves or the two disagree.
