<!-- markdownlint-disable MD060 -->
# Tiny-AI blob storage

The `model/tiny/` directory ships 28 ONNX model artefacts. Three of
them are large enough that we host them as attachments on a GitHub
Release rather than inlining them in git history (per ADR-0457):

| File                  | Size    | Source                                                                                  |
|-----------------------|---------|-----------------------------------------------------------------------------------------|
| `transnet_v2.onnx`    | 30.8 MB | `tiny-blobs-v1` release attachment                                                      |
| `fastdvdnet_pre.onnx` | 10.0 MB | `tiny-blobs-v1` release attachment                                                      |
| `lpips_sq.onnx`       | 3.3 MB  | `tiny-blobs-v1` release attachment                                                      |
| Other 25 files        | 12 MB   | inline in the repo (each ≤500 KB; per-file fetch overhead dominates below the 1 MB cutoff) |

## Fetching the blobs

The fetcher reads `model/tiny/registry.json`, downloads any blob
whose `release_url` field is non-empty AND whose local file is
missing, and verifies the recorded `sha256`:

```bash
scripts/ai/fetch-tiny-blobs.sh                 # download missing blobs
scripts/ai/fetch-tiny-blobs.sh --check         # verify present blobs only (no download)
scripts/ai/fetch-tiny-blobs.sh --force         # re-download even if present
scripts/ai/fetch-tiny-blobs.sh --release v1    # pin a specific release tag
```

Run it once after cloning. It is idempotent: re-running with
everything present is a no-op (~50 ms). Cold-cache fetch of all
three blobs is ~3.5 s on a typical home connection.

The fetcher has no third-party dependencies — it shells out to
`curl`, `jq`, and `sha256sum`. CI runners and dev containers all
have these out of the box.

## CI integration

CI workflows that touch the tiny-AI surfaces add a cache-warmed
fetch step early in the job:

```yaml
- name: Cache tiny-AI ONNX blobs
  uses: actions/cache@v4
  with:
    path: model/tiny/*.onnx
    key: tiny-blobs-${{ hashFiles('model/tiny/registry.json') }}

- name: Fetch tiny-AI blobs
  run: scripts/ai/fetch-tiny-blobs.sh
```

Cache key is the registry sha256 — any registry change (new model,
sha256 bump, new release_url) invalidates the cache automatically.

## Adding a new large model

When introducing a new ONNX file ≥ 1 MB:

1. Train / export the model into `model/tiny/<name>.onnx` locally.
2. Record `sha256` in `model/tiny/registry.json`.
3. Either roll a new `tiny-blobs-vN+1` release or attach to the
   current one:

   ```bash
   gh release upload tiny-blobs-v1 model/tiny/<name>.onnx \
     --repo VMAFx/vmafx
   ```

4. Add `release_url` to the registry entry pointing at the upload.
5. `git rm` the local file and let the fetcher serve it from the
   release.
6. PR the registry change. Reviewers verify by running the fetcher
   locally — `--check` should report the new file as missing,
   `--force` should download + verify it.

Files smaller than 1 MB stay inline in git — the per-file fetch
overhead dominates the per-byte storage savings below that
threshold. The cutoff is documented inline in the fetcher.

## Why not Git LFS

Git LFS was tried previously and removed in PR #846. The blobs were
declared as LFS-tracked in `.gitattributes` but never actually
uploaded to GitHub LFS storage, so `git checkout` in fresh worktrees
produced 130-byte LFS pointer text instead of the real binaries —
breaking every agent worktree we spawned. The current scheme stores
the blobs in regular release attachments, which are served from the
same origin without requiring `git lfs install` on the client.

## Why not Hugging Face

Hugging Face Hub was considered for the migration. Two reasons it
wasn't chosen for the initial pass:

- Adds an external runtime dependency (`huggingface_hub`) just to
  fetch three artefacts that already have stable URLs.
- The `model/tiny/` set is more "build artefact" than "shareable
  model"; discoverability isn't the gating concern at this scale.

If `vmaf_tiny_v*` upstream adoption grows, HF Hub mirroring is a
natural follow-up — the `release_url` field can point anywhere.
