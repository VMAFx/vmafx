- `scripts/ci/check-state-md-rows.sh` works with the `mawk` of Debian 12. That
  version reads regex intervals such as `{0,2}` literally, so the gate matched no
  bug row there and passed every `docs/state.md`, duplicates included. The gate
  now avoids intervals; CI's Ubuntu runner was not affected.
