- **CI workflow display names are short again** (ADR-1227). GitHub's
  `badge.svg` endpoint paints the workflow `name:` into the badge, so names
  like `Tests & Quality Gates — Netflix Golden / Sanitizers / Tiny AI /
  Coverage` (72 characters) rendered the README's seven status badges as
  60-to-70-character banners that wrapped the header across several lines.
  Fourteen workflows are relabelled — `Tests`, `Security`, `Builds`, `FFmpeg`,
  `E2E`, `Fuzz`, `SYCL Parity`, `Dev Container`, the two publish workflows and
  the three upstream watchers — bringing them under the ≤30-character budget
  `docs/development/ci-job-names.md` already applied to job names. The four the
  README badges point at now match their badge link labels exactly. The axis
  list each name used to carry moved to a comment under the `name:` line.
  Filenames are unchanged, so no badge URL churn and no branch-protection
  re-pin.
