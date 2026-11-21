<!-- markdownlint-disable MD046 MD060 -->
# Tiny-AI blob storage

Tiny-AI ONNX models live in git under `model/tiny/`. Today a plain `git clone`
gives you every model, and no fetch step is needed. This page explains the
current layout and the release-attachment mechanism of
[ADR-0457](../adr/0457-onnx-blobs-to-github-releases.md), which exists in the
tree but is not applied to any registry entry yet.

## Current state

`model/tiny/` holds 30 `.onnx` files (26 registry entries, some with
`.onnx.data` siblings), all tracked in git. Three are large:

| File | Size | Storage today |
| --- | --- | --- |
| `transnet_v2.onnx` | 30.8 MB | tracked in git |
| `fastdvdnet_pre.onnx` | 10.0 MB | tracked in git |
| `lpips_sq.onnx` | 3.3 MB | tracked in git |
| all other files | each at most 500 KB | tracked in git |

ADR-0457 decided to move the three large blobs to the `tiny-blobs-v1` GitHub
Release. That move is not done: no entry of
[`model/tiny/registry.json`](../../model/tiny/registry.json) carries a
`release_url` (the schema accepts the field, an `https://` URL).

## The fetcher

`scripts/ai/fetch-tiny-blobs.sh` reads `model/tiny/registry.json`, downloads
every blob whose `release_url` field is non-empty and whose local file is
missing, and verifies the recorded `sha256`. It needs only `curl`, `jq` and
`sha256sum`.

```bash
scripts/ai/fetch-tiny-blobs.sh           # download missing blobs
scripts/ai/fetch-tiny-blobs.sh --check   # verify present blobs only, no download
scripts/ai/fetch-tiny-blobs.sh --force   # re-download even if present
```

The script defines no other flag. With the current registry it prints
`no release-hosted blobs in registry; nothing to do.` and exits 0. It is
idempotent: with everything present it is a no-op.

!!! note
    No CI workflow calls the fetcher today. The cache-warmed fetch step that
    ADR-0457 sketches is a design, not an applied job:

    ```yaml
    - name: Cache tiny-AI ONNX blobs
      uses: actions/cache@v4
      with:
        path: model/tiny/*.onnx
        key: tiny-blobs-${{ hashFiles('model/tiny/registry.json') }}

    - name: Fetch tiny-AI blobs
      run: scripts/ai/fetch-tiny-blobs.sh
    ```

    The cache key is the registry hash, so any registry change (new model,
    `sha256` bump, new `release_url`) would invalidate the cache.

## Host a new large model as a release attachment

Use this only when you apply ADR-0457 to a file of 1 MB or more. Smaller files
stay inline in git, because per-file fetch overhead dominates below that size.

1. Export the model to `model/tiny/<name>.onnx` locally.
2. Record `sha256` in `model/tiny/registry.json`.
3. Roll a new `tiny-blobs-vN+1` release or attach to the current one:

    ```bash
    gh release upload tiny-blobs-v1 model/tiny/<name>.onnx \
      --repo vidiko/vmafx
    ```

4. Add `release_url` to the registry entry, pointing at the upload (see
   [model-registry.md](model-registry.md) for the field).
5. `git rm` the local file so the fetcher serves it from the release.
6. Open the PR. Reviewers run the fetcher locally: `--check` should report the
   new file as missing and `--force` should download and verify it.

## Why not Git LFS

Git LFS was tried and removed in PR #846. The blobs were declared LFS-tracked in
`.gitattributes` but never uploaded to GitHub LFS storage, so `git checkout` in
fresh worktrees produced 130-byte pointer text instead of the binaries. The
`.gitattributes` now marks `model/tiny/*.onnx` as plain binary.

## Why not Hugging Face

Hugging Face Hub was considered and not chosen for the initial pass:

- It adds an external runtime dependency (`huggingface_hub`) to fetch three
  artefacts that already have stable URLs.
- The `model/tiny/` set is a build artefact more than a shareable model, so
  discoverability is not the gating concern at this scale.

If `vmaf_tiny_v*` adoption grows, mirroring on the Hub is a natural follow-up:
the `release_url` field can point anywhere.
