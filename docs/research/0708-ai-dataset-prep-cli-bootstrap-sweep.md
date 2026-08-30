# Research 0708: AI Dataset-Prep CLI Bootstrap Sweep

## Question

Which dataset-prep AI scripts still carried local direct-invocation setup after
the shared AI bootstrap and CLI helpers landed?

## Inputs Reviewed

- `ai/scripts/fetch_youtube_ugc_subset.py`
- `ai/scripts/fetch_konvid_1k.py`
- `ai/scripts/extract_ugc_features.py`
- `ai/scripts/extract_konvid_frames.py`
- `ai/scripts/build_bisect_cache.py`
- `ai/scripts/collect_gpu_calibration_data.py`
- `ai/tests/test_dataset_fetch_manifests.py`
- `ai/tests/test_legacy_extractor_manifests.py`
- `ai/tests/test_build_bisect_cache.py`
- `ai/tests/test_extract_ugc_features.py`

## Findings

These fetch/prep scripts already wrote replay manifests, but they still kept
copies of the same local scaffolding:

- direct `sys.path.insert(...)` blocks for `ai/src` or the repository root;
- direct `argparse.ArgumentParser(...)` construction;
- local `sys.argv[1:]` or full `sys.argv` capture for provenance;
- `Path(__file__)` entrypoint values instead of the resolved script path
  returned by the shared bootstrap helper.

`extract_ugc_features.py` is the only script in this group that needs the
repository root on `sys.path`, because it imports `ai.data.feature_extractor`
when invoked as `python ai/scripts/extract_ugc_features.py`. The others only
need `ai/src` for `aiutils`.

## Scope Chosen

Migrate the group to:

- `bootstrap_ai_script(__file__)` or
  `bootstrap_ai_script(__file__, include_repo_root=True)`;
- `make_argument_parser(...)`;
- `collect_cli_argv(argv)`;
- normalized `run_provenance["argv"]` values for fetch manifests and
  extraction manifests.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_dataset_fetch_manifests.py \
  ai/tests/test_legacy_extractor_manifests.py \
  ai/tests/test_build_bisect_cache.py \
  ai/tests/test_extract_ugc_features.py -q
```

## Limits

This is a script hygiene sweep only. It does not download datasets, extract
features, regenerate bisect fixtures, or change output schemas.
