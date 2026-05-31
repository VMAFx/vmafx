- **CI scripts (Round 26 audit D.1 + D.2)**: fix `assertion-density.sh` to
  match post-rebrand `Copyright YYYY Lusoris` headers in addition to the
  legacy `Lusoris and Claude (Anthropic)` form — prevents silent skip of the
  Power of 10 rule-5 gate after the copyright rebrand lands ([ADR-0968]).
  Add `trap 'rm -f "$tmp_body" "$tmp_out"' EXIT` to
  `concat-changelog-fragments.sh --write` so both tempfiles are cleaned up
  under any exit path including awk pipeline failure ([ADR-0968]).
