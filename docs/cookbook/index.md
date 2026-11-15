<!-- markdownlint-disable MD013 MD046 MD060 -->
# Cookbook

Short, tested recipes for specific situations. Every command block tagged
`run-check` is executed by the docs check
(`scripts/docs/check_docs_commands.py`) against the fixture pair wherever its
toolchain exists; numbers are only quoted where a recorded source pins them.

The fixture pair is `testdata/ref_576x324_48f.yuv` /
`testdata/dis_576x324_48f.yuv` (576x324, 48 frames, yuv420p, 8-bit), scored
with `vmaf_v0.6.1` on the CPU — the
[pooled mean](../reference/concepts.md) of that run is 94.32301
(`testdata/scores_cpu_576.json`).

## Recipes

| Recipe | Situation |
|---|---|
| [Score a 10-bit pair](10-bit.md) | your inputs are 10-bit (or you want 10-bit scoring) |
| [Score with a frame offset](frame-offset.md) | the distorted stream starts late or early |
| [Score non-420 subsampling](subsampling.md) | 4:2:2 / 4:4:4 inputs |
| [Score a batch of pairs](batch.md) | many pairs, one report each |
| [Score on two GPUs](two-gpus.md) | pick which device runs the scoring |

The [tutorials](../tutorials/index.md) teach the basics in order; the
[chooser](../reference/chooser.md) picks the metric; the
[glossary](../reference/glossary.md) defines the terms.
