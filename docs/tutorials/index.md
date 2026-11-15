<!-- markdownlint-disable MD013 MD046 MD060 -->
# Tutorials

Five short tutorials, ten minutes each, in order. Every command runs against
the fixture pair already in the tree — `testdata/ref_576x324_48f.yuv` and
`testdata/dis_576x324_48f.yuv` (576x324, 48 frames, yuv420p, 8-bit) — so a
CPU build is enough; no GPU or special hardware is needed. Each tutorial
ends in a result you can check against a stated number or file, and every
command block tagged `run-check` is executed by the docs check
(`scripts/docs/check_docs_commands.py`, wired into `make docs-fragments-check`)
wherever its toolchain exists — a changed flag name fails the check.

Start with [Score a pair](score-a-pair.md). If you have not built the tools
yet, follow [getting started](../getting-started/index.md) first — a plain
CPU meson build produces the `vmaf` binary the tutorials use.

| # | Tutorial | You will have |
|---|---|---|
| 1 | [Score a pair](score-a-pair.md) | a JSON report with the pooled VMAF score |
| 2 | [Compare two encodes](compare-encodes.md) | two scores and a verdict |
| 3 | [Choose a target](choose-a-target.md) | the CRF that hits a target score |
| 4 | [Gate a build](gate-a-build.md) | a pass/fail quality gate script |
| 5 | [Score live](score-live.md) | scores for every encode in a loop |

The [cookbook](../cookbook/index.md) holds the short recipes for specific
situations (10-bit, offsets, subsampling, batch); the
[concept guide](../reference/concepts.md) explains what the numbers mean; the
[FAQ](../reference/faq.md) answers the odd ones.

All tutorial numbers are stated for `vmaf_v0.6.1` on the CPU backend — the
model the golden-data tests pin — so your runs should match them. The fork's
default model gives different numbers by design
([why](../reference/concepts.md#model-mismatch)).
