<!-- markdownlint-disable MD013 -->
# Research 0713: CHUG HDR CLI Bootstrap Sweep

Date: 2026-05-22

## Scope

Apply the accepted AI script helper pattern to the CHUG HDR entrypoints:

- `ai/scripts/chug_extract_features.py`
- `ai/scripts/train_chug_hdr_mos_head.py`

The goal is to keep the HDR path aligned with the rest of the AI tooling while the long K150K
feature refresh continues separately.

## Findings

`chug_extract_features.py` still derived `SCRIPT_PATH`, `REPO_ROOT`, and `ai/src` manually, then
captured `sys.argv[1:]` directly before building the run-provenance sidecar.

`train_chug_hdr_mos_head.py` manually inserted the AI scripts directory so it could delegate to
`train_konvid_mos_head.py`. It also parsed forwarded arguments from the original `argv` object
while separately constructing a raw argv list for the delegated `--run-argv-json` payload.

## Change

Both scripts now use:

- `bootstrap_ai_script(...)` for path setup
- `collect_cli_argv(...)` for the exact CLI vector recorded in manifests
- `make_argument_parser(...)` for consistent AI script parser formatting

The CHUG MOS wrapper uses `include_ai_scripts=True` and forwards the same collected argv that it
records in the delegated manifest payload.

## Alternatives Considered

| Option | Result | Rationale |
| --- | --- | --- |
| Leave CHUG alone until the current K150K run completes | Rejected | These scripts are independent from the running K150K extractor and are HDR-priority backlog. |
| Inline CHUG training instead of delegating to the KonViD MOS trainer | Rejected | That would be a behavior change and a larger training-policy decision; this PR only removes boilerplate drift. |
| Add a CHUG-specific parser helper | Rejected | The generic AI helper covers the needed path setup and provenance capture. |

## Validation

Focused tests:

```bash
.venv/bin/python -m pytest ai/tests/test_chug.py ai/tests/test_train_konvid_mos_head.py -q
```

Static checks:

```bash
.venv/bin/ruff check ai/scripts/chug_extract_features.py ai/scripts/train_chug_hdr_mos_head.py
.venv/bin/black --check ai/scripts/chug_extract_features.py ai/scripts/train_chug_hdr_mos_head.py
```

The end-to-end `ai/tests/test_chug_extract_features_smoke.py` run was attempted locally, but the
host `build/tools/vmaf` process stopped making progress after writing the temporary JSON output.
That smoke covers the libvmaf binary path rather than this bootstrap-only diff, so it is left out
of this PR's required validation.

## Rebase Notes

No rebase-sensitive invariant changes. The wrapper and extractor retain their existing output
schemas and only adopt the shared AI helper pattern.
