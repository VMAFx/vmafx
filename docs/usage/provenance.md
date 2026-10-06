# Score provenance

Every score VMAFx reports carries a provenance record: which library, ABI and
build computed it, on which device, with which model (and the SHA-256 of the
model bytes), with which options, on which frames, and which extractor wrote
each feature. The record is the same everywhere: the C API, the `vmaf`
reports, the scoring server's responses and the MCP tools. Its digest covers
the configuration and every score, so a report can be checked for changes and
re-run bit for bit later ([ADR-2073](../adr/2073-vmafx-provenance-record.md),
issue #2142).

## Quick start

```bash
vmaf -r ref.yuv -d dist.yuv -w 1920 -h 1080 -p 420 -b 8 \
     --json -o report.json --precision max
vmaf --verify-provenance report.json
# provenance verified: every configuration field and score of the re-run is identical
```

`--verify-provenance` re-runs the command line the report recorded (from the
directory you run it in, so relative input paths must resolve there), writes
the re-run to `report.json.rerun.json` and compares the two. It exits with:

| Exit | Meaning |
| --- | --- |
| 0 | Every configuration field and every score is identical; the re-run report is removed. |
| 1 | A field or score differs. The message names the first one and both values, for example `provenance mismatch: frames[3].metrics.vmaf: recorded 88.838202, re-run 88.838201`; the re-run report is kept for inspection. |
| 2 | The check could not run: the report is not JSON, records no command line, or the re-run failed. |

Fields that describe where and when a report was made rather than what was
scored may differ between the two runs without failing the check: `version`,
`commit`, `build_id`, `compiler`, `build_flags`, `fp_policy`, `backends`,
`rust_twins`, `simd`, the device fields, `elapsed_ns`, and each feature's
`device`, `runtime` and `exactness`. Scores must be identical whatever the
build; a twin listed as `exact` returns the CPU's bits on every backend.

Use `--precision max` for reports you intend to verify: a lossless report also
lets the check recompute the score digest from the report alone.

## What the reports carry

| Format | Provenance |
| --- | --- |
| JSON | A `provenance` object (below), a `score_format` member (the printf format of the scores) and the backend receipt `backend_used` / `feature_backends` ([CLI reference](cli.md#backend-receipt-in-json-output)). |
| XML | One `<provenance>` element inside `<VMAF>`: one attribute per field, a `<model>`, `<feature>` or `<annotation>` child per item. |
| CSV, SUB | Unchanged. Pass `--provenance-sidecar` to also write the record to `<output>.provenance.json`. |

The JSON record is the proto JSON mapping of the `Provenance` message
(`proto/vmafx_api.proto`): field names as keys, 64-bit integers as strings,
enumerations by their lower-case value names. An abbreviated example:

```json
"provenance": {"abi_major":0,"abi_minor":1,"abi_patch":5,"active_backend":"cpu",
  "annotations":[{"key":"cli_argv","value":"-r"},{"key":"cli_argv","value":"ref.yuv"}],
  "backends":"cpu","bpc":8,
  "build_flags":"buildtype=release optimization=3 debug=false b_ndebug=false b_lto=false c_args= cpp_args=",
  "build_id":"sha256:fba9b05d...","commit":"4b7e56d9...","compiler":"c=gcc 16.2.1 cpp=gcc 16.2.1",
  "cpumask":"0","device_index":0,"device_name":"cpu","device_runtime":"x86_64",
  "digest":"sha256:73a5979d...","elapsed_ns":"5171524","encode_record":"",
  "features":[{"backend":"cpu","device":"cpu","exactness":"cpu-reference","extractor":"adm",
               "feature":"integer_adm2","implementation":"c","options":"adm_enhn_gain_limit=1",
               "runtime":"x86_64","source":"extractor"}],
  "fp_policy":"-ffp-contract=off","frame_height":1080,"frame_width":1920,"gpumask":"0",
  "models":[{"flags":"0","name":"vmaf","overrides":"",
             "sha256":"e4cf8c14...","version":"vmaf_v1.0.16_3d0h"}],
  "n_annotations":14,"n_extractors":4,"n_features":15,"n_frames":"48","n_models":1,
  "n_subsample":1,"n_threads":0,"pix_fmt":"yuv420p","rust_twins":0,
  "scores_digest":"sha256:260af289...","simd":"avx512icl","version":"v1.0.0-rc.2-533-g4b7e56d9e"}
```

## Fields

The generated [API reference](../api/vmafx/provenance.md) documents every field;
by group:

| Group | Fields |
| --- | --- |
| Library | `version` (`git describe`), `commit`, `abi_major` / `abi_minor` / `abi_patch` |
| Build | `build_id` (digest of the next five), `compiler`, `build_flags`, `fp_policy` (the strict floating-point arguments, [ADR-1461](../adr/1461-strict-fp-every-translation-unit.md)), `backends`, `rust_twins`; `simd` is the instruction-set level the CPU extractors dispatch to under the cpumask |
| Device | `active_backend`, `device_index`, `device_name`, `device_runtime` |
| Options | `n_threads`, `n_subsample`, `cpumask`, `gpumask`, `n_extractors` |
| Input | `frame_width`, `frame_height`, `pix_fmt`, `bpc`, `n_frames`; the CLI's command line as `cli_argv` annotations |
| Models | `models[]`: `name`, `version` (built-in version or file path), `sha256` of the model bytes as loaded, `flags`, `overrides` (feature options applied after loading) |
| Features | `features[]`: `feature` (the name the report uses), `extractor`, `implementation` (`c` or `rust`), `backend`, `device`, `runtime`, `options`, `exactness`, `source` (`extractor`, `model`, `imported`) |
| Encode | `encode_record`: the digest of the encode record of the distorted input ([VMAFx/pelorus#81](https://github.com/VMAFx/pelorus/issues/81)), empty until a producer sets it |
| Digests | `scores_digest`, `digest`; `elapsed_ns` is timing and not digested |

`exactness` says how a feature's scores relate to the CPU extractor's:
`cpu-reference` (the CPU extractor itself), `exact` (a twin declared
bit-identical in `scripts/ci/exact_twins.d`), `libm-bounded <bound>` (differs
only through the math library, bounded), `tolerance <bound>` (the parity
gate's bound), or `unclassified` for a device extractor the gate does not
list. The table is generated by `scripts/codegen/vmafx_exactness.py`.

## Digests

`digest` is `sha256:` and the SHA-256 of the record's canonical JSON without
`digest` and `elapsed_ns`. The canonical form is RFC 8785: members in byte
order of their keys, no whitespace, integers in plain decimal, strings escaping
only `"`, `\` and control characters. For this record it equals Python's
`json.dumps(record, sort_keys=True, separators=(",", ":"), ensure_ascii=False)`:

```python
import hashlib, json
record = json.load(open("report.json"))["provenance"]
body = {k: v for k, v in record.items() if k not in ("digest", "elapsed_ns")}
text = json.dumps(body, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
assert record["digest"] == "sha256:" + hashlib.sha256(text.encode()).hexdigest()
```

`scores_digest` is the SHA-256 of one line `<feature> <frame> <bits>\n` per
score, `<bits>` being the 16 lower-case hex digits of the score's IEEE-754
double, features in byte order of their report names and frames in order. As
it is part of the digested record, `digest` changes when any score does.

## C API

```c
VmafxProvenance record = VMAFX_PROVENANCE_INIT;
vmafx_context_provenance(context, &record, NULL);          /* scalars, digests */
VmafxModelProvenance model = VMAFX_MODEL_PROVENANCE_INIT;
vmafx_context_model_provenance(context, 0, &model, NULL);  /* also features, annotations */
vmafx_context_annotate(context, "input_ref", "ref.yuv", NULL);
vmafx_context_set_encode_record(context, "sha256:...", NULL);
const char *json = NULL;
vmafx_context_provenance_json(context, VMAFX_PROVENANCE_JSON_CANONICAL, &json, NULL);
vmafx_report_write(context, "report.json", VMAFX_REPORT_FORMAT_JSON, 0, "%.17g", NULL);

VmafxReportFile *recorded = NULL, *rerun = NULL;
vmafx_report_open("report.json", &recorded, NULL);
vmafx_report_open("rerun.json", &rerun, NULL);
VmafxError *error = NULL;
if (vmafx_report_verify(recorded, rerun, &error) == VMAFX_E_MISMATCH)
    printf("%s: %s\n", vmafx_error_subject(error), vmafx_error_message(error));
```

`vmafx_report_verify(recorded, NULL, ...)` checks one report on its own: its
digest, and its score digest when it is lossless.

## Server and MCP

The scoring server copies the record into the `library` member of every
response's `ScoreProvenance` ([scoring API contract](../server/api-contract.md));
both MCP servers return it in the `provenance` member of a JSON result, the
direct path included.
