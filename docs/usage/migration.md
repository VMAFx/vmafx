<!-- markdownlint-disable MD013 MD046 MD060 -->
# Migration guides from upstream tools

Users arrive at VMAFx with an upstream `vmaf` command, an FFmpeg `libvmaf`
filter string, or a wrapper script driving them. This page maps each of those
to the VMAFx equivalent, states what is the same and what differs, and names
the one number every equivalent command must produce on the shared fixture
pair. The FFmpeg-side option renaming is already tabled on
[Using VMAF with FFmpeg](ffmpeg.md#migrating-from-the-libvmaf-filters); this
page covers the whole-command move from four starting points:

- [the upstream `vmaf` CLI and Python harness](#from-the-upstream-vmaf-cli-and-python-harness),
- [the stock upstream FFmpeg `libvmaf` filter](#from-the-ffmpeg-libvmaf-filter),
- [`ffmpeg-quality-metrics`](#from-ffmpeg-quality-metrics),
- [`easyVmaf`](#from-easyvmaf).

## How the equivalence rows are checked

Every row carries a check id, an expected pooled mean, and a status. The
equivalence fixture is the golden pair already in-tree,
`testdata/ref_576x324_48f.yuv` and `testdata/dis_576x324_48f.yuv`
(576x324, 48 frames, yuv420p, 8-bit), scored with `vmaf_v0.6.1` on the CPU
backend. `testdata/scores_cpu_576.json` records that run: the pooled mean
every equivalent row must land on is **94.32301**.

`scripts/docs/check_migration_equivalence.py` holds the page to that number
(`make docs-fragments-check`): a quoted number that drifts from the snapshot,
an equivalent row without a command block, or a not-equivalent row without a
reason and an ADR link fails the docs check, with no binary needed. Rows are
tagged `<!-- migration-check:<id> exec=<class> -->` above their command block;
class `cli` rows are executed against the fixture pair wherever a built binary
exists (`--execute`, run nightly in the `netflix-benchmark` job), and the
`python`, `ffmpeg` and `wrapper` rows execute only where their toolchain is
present, skipping with a printed reason otherwise. A `wrapper` row whose tool
is present but whose wrapped fixture pair is missing fails instead of
skipping — build the containers with the wrap commands in
[the `ffmpeg-quality-metrics` section](#from-ffmpeg-quality-metrics).

## From the upstream `vmaf` CLI and Python harness

The `vmaf` binary is upstream Netflix's CLI and the fork keeps its flags
and scores compatible when the model is pinned: pass
`-m version=vmaf_v0.6.1` as the commands below do. Unpinned, both `vmaf`
and `vmafx` default to `vmaf_v1.0.16_3d0h`, a different score; `vmafx` is
the modernized twin with different defaults ([vmafx CLI](vmafx-cli.md)). The upstream Python harness
(`python -m vmaf.script.run_vmaf`, [Python library](python.md)) is retained
unchanged in the fork's compatibility package.

Upstream on the fixture pair (reference first, JSON pooled mean of
`vmaf_v0.6.1`):

```bash
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m version=vmaf_v0.6.1 --json -o /dev/stdout
```

Upstream Python harness on the same pair (positional: format, width, height,
reference, distorted — reference first here too):

```bash
python -m vmaf.script.run_vmaf yuv420p 576 324 \
    testdata/ref_576x324_48f.yuv testdata/dis_576x324_48f.yuv --out-fmt json
```

| Check id | Expected pooled mean | Status | Notes |
|---|---|---|---|
| `cli-vmafx-default` | `94.32301` | equivalent | `vmafx` defaults to `--precision=max`, so the JSON carries full-precision scores; the pooled mean matches the `vmaf` run. Output precision differences are ADR-0690. |
| `cli-netflix-compat` | `94.32301` | equivalent | `vmafx --netflix-compat` restores the four upstream defaults — `.6f` scores, the upstream banner, the CPU backend, and `vmaf_v0.6.1` (ADR-0696) — so the score text matches; the JSON still carries the fork's `wall_fps` and provenance record. |
| `python-run-vmaf` | `94.32301` | equivalent | The harness reports `aggregate.VMAF_score` (mean pool) with the same default model; the number matches the CLI row. |

The VMAFx equivalents the check executes (`VMAF_BIN` overrides the binary
name; it must be absolute, and `--backend cpu` pins the snapshot's backend):

<!-- migration-check:cli-vmafx-default exec=cli -->
```bash
vmafx -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m version=vmaf_v0.6.1 \
    --json -o /dev/stdout --backend cpu
```

<!-- migration-check:cli-netflix-compat exec=cli -->
```bash
vmafx --netflix-compat -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m version=vmaf_v0.6.1 \
    --json -o /dev/stdout --backend cpu
```

<!-- migration-check:python-run-vmaf exec=python -->
```bash
python -m vmaf.script.run_vmaf yuv420p 576 324 \
    testdata/ref_576x324_48f.yuv testdata/dis_576x324_48f.yuv --out-fmt json
```

## From the FFmpeg `libvmaf` filter

Stock FFmpeg scores through the `libvmaf` filter; the fork's patch series
replaces it with the `vmafx` filter ([The vmafx filter](ffmpeg.md#the-vmafx-filter)).
The full option-by-option mapping lives in
[the surface mapping table](ffmpeg.md#surface-mapping-table); the traps that
bite a moved command:

- **Input pads are reversed**: FFmpeg pad 0 is the distorted video, pad 1 the
  reference — the opposite of the CLI and Python surfaces above
  ([Input ordering](ffmpeg.md#input-ordering)).
- The fork filter pairs frames by timestamp and passes unpaired main frames
  unscored instead of repeating the last reference frame
  ([Pairing the two inputs](ffmpeg.md#pairing-the-two-inputs)).
- The fork's retired Vulkan filter and its `vulkan_device` option have no
  replacement; see [the stock upstream libvmaf filter migration table](ffmpeg.md#migrating-from-the-libvmaf-filters)
  (ADR-0726, ADR-0860, ADR-2166).

Upstream filter command on the fixture pair (`log_fmt=json` to stdout):

<!-- migration-check:ffmpeg-upstream exec=ffmpeg -->
```bash
ffmpeg -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv \
    -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv \
    -f null -lavfi "[0:v][1:v]libvmaf=model=version=vmaf_v0.6.1:log_fmt=json:log_path=/dev/stdout" -  # stock upstream libvmaf filter
```

The VMAFx equivalent renames the filter to `vmafx`; with the same model the
pooled mean matches the CLI row (`model=version=vmaf_v0.6.1` is required —
the filter's default model is `vmaf_v1.0.16_3d0h`, a different score):

<!-- migration-check:ffmpeg-libvmaf exec=ffmpeg -->
```bash
ffmpeg -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv \
    -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv \
    -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1:log_fmt=json:log_path=/dev/stdout" \
    -f null -
```

| Check id | Expected pooled mean | Status | Notes |
|---|---|---|---|
| `ffmpeg-upstream` | `94.32301` | equivalent | Stock upstream FFmpeg `libvmaf` filter; this is the row the FFmpeg integration job executes. |
| `ffmpeg-libvmaf` | `94.32301` | equivalent | Same engine and model behind both filters; the fork build needs `--enable-libvmafx` (ADR-1852, ADR-2166). |

## From `ffmpeg-quality-metrics`

`ffmpeg-quality-metrics` (slhck, MIT) drives stock FFmpeg's `libvmaf` filter
and pools nothing itself: it reports per-frame scores (JSON by default).
The equivalent VMAFx run is the `vmafx` CLI directly on a raw pair, or the
`vmafx` filter inside FFmpeg above.

Both wrapper tools drive FFmpeg, which cannot read headerless raw `.yuv`; the
fixture pair is wrapped losslessly first (FFV1 decodes bit-identical, so every
score is unchanged). The wrap commands produce `testdata/migration-fixture/`:

```bash
mkdir -p testdata/migration-fixture && \
ffmpeg -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv -c:v ffv1 -level 3 \
    testdata/migration-fixture/dis.mkv && \
ffmpeg -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -c:v ffv1 -level 3 \
    testdata/migration-fixture/ref.mkv
```

Upstream on the wrapped pair (distorted first — and note the model pin; the
tool's default model is the libvmaf default, not `vmaf_v0.6.1`, and the model
file ships with your libvmaf install under `share/model/`):

```bash
ffmpeg-quality-metrics testdata/migration-fixture/dis.mkv \
    testdata/migration-fixture/ref.mkv \
    -m vmaf --vmaf-model-path /usr/local/share/model/vmaf_v0.6.1.json \
    --threads 4 -of json
```

| Check id | Expected pooled mean | Status | Notes |
|---|---|---|---|
| `fqm-vmaf` | `94.32301` | equivalent | Same libvmaf core, so per-frame scores match; the pooled mean above is the mean of the tool's per-frame JSON. Argument order is reversed against the CLI (distorted first, reference second). |

The VMAFx equivalent (`cli-vmafx-default` in
[the first section](#from-the-upstream-vmaf-cli-and-python-harness) is the
command; its per-frame JSON mean is the row's number):

<!-- migration-check:fqm-vmaf exec=wrapper -->
```bash
ffmpeg-quality-metrics testdata/migration-fixture/dis.mkv \
    testdata/migration-fixture/ref.mkv \
    -m vmaf --vmaf-model-path /usr/local/share/model/vmaf_v0.6.1.json \
    --threads 4 -of json
```

## From `easyVmaf`

`easyVmaf` (gdavila, MIT) wraps FFmpeg's `libvmaf` with automatic
preprocessing: deinterlacing, display scaling, pixel-format normalization and
frame-accurate sync search. On inputs that are already aligned and identical
in format it is a plain `libvmaf` run, and the VMAFx equivalent is the
`vmafx` CLI on the same pair.

Upstream on the wrapped pair (distorted `-d` first, reference `-r` second; the
wrap commands are in [the previous section](#from-ffmpeg-quality-metrics)):

```bash
easyvmaf -d testdata/migration-fixture/dis.mkv -r testdata/migration-fixture/ref.mkv
```

| Check id | Expected pooled mean | Status | Notes |
|---|---|---|---|
| `easyvmaf-aligned` | n/a | not equivalent | easyVmaf 3.0.0's default HD mode rescales both fixtures to 1920x1080 before scoring, even when the inputs have equal dimensions; easyVmaf 4.0.0 also defaults to `vmaf_v1`, not `vmaf_v0.6.1`. The native-resolution `vmafx` CLI therefore does not preserve this scoring configuration ([easyVmaf 3.0.0 model](https://raw.githubusercontent.com/gdavila/easyVmaf/v3.0.0/easyvmaf/vmaf.py), [easyVmaf 4.0.0 model](https://raw.githubusercontent.com/gdavila/easyVmaf/v4.0.0/README.md), ADR-2165). Use an explicit FFmpeg scale/filter chain and pin the model before scoring with `vmafx`. |
| `easyvmaf-auto-prep` | n/a | not equivalent | The automatic deinterlace/scale/sync/fps preprocessing has no VMAFx equivalent flag: the engine scores the pair it is given and warns on unpaired frames instead of aligning them ([Pairing the two inputs](ffmpeg.md#pairing-the-two-inputs), ADR-2165). Do the preprocessing in an FFmpeg chain first, then score with `vmafx`. |

The VMAFx equivalent for the aligned row is not the native CLI: the input must be
scaled and the model pinned first, then scored with `cli-vmafx-default`.

## Summary of what differs

| Moving from | Same | Differs in VMAFx |
|---|---|---|
| `vmaf` CLI | flags and scores with the model pinned | `vmafx` defaults (`--precision=max`, banner, ADR-0690); `--netflix-compat` restores `.6f`, banner, CPU backend and `vmaf_v0.6.1` (ADR-0696) |
| Python harness | `run_vmaf` module path and flags | nothing to move; the `vmafx` bindings package ([Python package](python-package.md)) is the modernized C API alternative |
| FFmpeg `libvmaf` | pad meaning, model spelling | filter renamed `vmafx`, some options renamed (mapping table), pairing defaults stricter, Vulkan dropped (ADR-0726, ADR-2166) |
| `ffmpeg-quality-metrics` | libvmaf scores | tool drives stock FFmpeg; call `vmafx` directly instead; distorted-first order |
| `easyVmaf` | libvmaf scores after explicit alignment and model pinning | default HD rescaling and `vmaf_v1` default are not preserved; auto-preprocessing not equivalent (ADR-2125; pairing contract ADR-2165); align and pin first, then score |
