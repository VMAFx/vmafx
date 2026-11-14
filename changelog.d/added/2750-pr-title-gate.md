- **Pull-request titles are checked in CI by the existing squash-message gate, and the
  `type:*` label now derives from the declared types (#2750).** The title becomes the
  subject of the squashed commit on `master`, and the required `DCO Sign-off` job
  already checked that subject on every pull request via
  `scripts/ci/squash_message.py check` — but the composed subject `<title> (#N)` let
  two defects through that the pinned engine cannot see: a title with an empty
  conventional subject (`fix:`) passed once the ` (#N)` suffix filled the description
  slot, and a type the hook refuses because `.pre-commit-config.yaml` does not declare
  it (such as `style`) passed on the engine's own type list. The check now refuses
  both, alongside the hook's own refusals (unknown type, missing delimiter, breaking
  change without `!` or `Migration:`). The PR-type label workflow drops its own copy
  of the type list and labels through the new `squash_message.py emit-label`, which
  reads `declared_types()` and prints `type:<type>`; `.github/release.yml` gains
  `type:feat`/`type:fix` section aliases, drops the `type:security`/`type:style`
  entries (no pull request has ever carried either label), and `docs/principles.md`
  names the CI check next to the hook.
