# Research 0704: AI Corpus Adapter CLI Helper Sweep

## Question

Which corpus JSONL adapters still duplicate direct-invocation path setup,
parser construction, and raw-argument provenance capture after ADR-0680 and
ADR-0681 introduced shared helpers?

## Inputs Reviewed

- `ai/scripts/chug_to_corpus_jsonl.py`
- `ai/scripts/konvid_1k_to_corpus_jsonl.py`
- `ai/scripts/konvid_150k_to_corpus_jsonl.py`
- `ai/scripts/youtube_ugc_to_corpus_jsonl.py`
- `ai/scripts/waterloo_ivc_to_corpus_jsonl.py`
- `ai/scripts/lsvq_to_corpus_jsonl.py`
- `ai/scripts/live_vqc_to_corpus_jsonl.py`
- `ai/scripts/bvi_dvc_to_corpus_jsonl.py`
- `ai/src/aiutils/cli_helpers.py`
- `ai/scripts/_script_bootstrap.py`

## Findings

The MOS/feature materializer family already moved to the shared helper shape,
but the corpus JSONL adapters still carried older idioms:

- direct `argparse.ArgumentParser(...)` creation in every adapter;
- local `sys.argv[1:]` capture for manifest provenance;
- repeated `Path(__file__).resolve().parents[2]` repo-root derivation;
- in the BVI-DVC adapter, hand-written `sys.path.insert(...)` blocks for
  `tools/vmaf-tune/src` and `ai/src`.

The helpers are behavior-preserving for these scripts. They keep the same CLI
flags, output schemas, default paths, and manifest schemas while making direct
script execution and run-provenance capture match the rest of the AI stack.

## Scope Chosen

Migrate the active corpus JSONL adapter group to:

- `bootstrap_ai_script(__file__)` or `include_vmaf_tune_src=True` where the
  adapter imports `vmaftune`;
- `make_argument_parser(...)`;
- `collect_cli_argv(argv)`;
- bootstrap-provided `SCRIPT_PATH` / `REPO_ROOT` values for manifest provenance.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_chug.py \
  ai/tests/test_konvid_1k.py \
  ai/tests/test_konvid_150k.py \
  ai/tests/test_youtube_ugc.py \
  ai/tests/test_waterloo_ivc.py \
  ai/tests/test_lsvq.py \
  ai/tests/test_live_vqc.py \
  ai/tests/test_legacy_corpus_extraction_manifests.py -q
```

## Limits

This sweep does not migrate every training/export/evaluation script. The
remaining scripts should move in owner-family batches so tests and report
schemas stay easy to reason about.
