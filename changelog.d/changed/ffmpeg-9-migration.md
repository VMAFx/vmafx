- **FFmpeg moves from n8.1.1 to n9.0.1** (released 2026-08-12). All seventeen
  patches in `ffmpeg-patches/` apply cleanly to the new base at full context; two
  needed regenerating for line drift only — `0002-add-vmaf_pre-filter` and
  `0008-add-libvmaf_tune-filter`, the latter because FFmpeg 9 inserted
  `CONFIG_FRC_AMF_FILTER` into the trailing context of its `libavfilter/Makefile`
  hunk. **No API changed and no patch was dropped**: upstream
  `libavfilter/vf_libvmaf.c` is byte-identical across n8.1.1, n9.0.1 and master
  (blob `3ab67d3d`), so upstream has absorbed none of the fork's filter options.
- **`FFMPEG_TAG=n8.2` was a fiction and is retired.** `docker/Dockerfile.node`
  and `.github/workflows/docker-publish-operator-node.yml` pinned a tag that has
  never existed upstream — there is no `refs/tags/n8.2` and no
  `refs/heads/release/8.2`, only an `n8.2-dev` in-development marker. Since
  `Dockerfile.node` clones with `--branch "${FFMPEG_TAG}"`, that image could
  never have built. It went unnoticed because the publishing workflow has zero
  recorded runs. `docs/state.md`'s claim that the node images "ship ffmpeg n8.2
  compiled from source" was false and is corrected.
- Every FFmpeg reference now standardises on the `n9.0.1` **tag**, resolving the
  previous split where Dockerfiles tracked tags while CI tracked the
  `release/8.1` branch head: `Dockerfile`, `Dockerfile.ffmpeg`,
  `docker/Dockerfile.node`, `ffmpeg-patches/series.txt`,
  `ffmpeg-patches/test/build-and-run.sh`, `scripts/ci/ffmpeg-patches-check.sh`,
  `.github/workflows/ffmpeg-integration.yml`,
  `.github/workflows/docker-publish-operator-node.yml` and the HIP-hwdec watcher.
