`vmaf-tune compare` / `tune-per-shot`: fix BBB v13 1080p ENOSPC caused by
3 concurrent codec bisects each materialising a 110 GB reference YUV decode
in parallel (330 GB peak on a 420 GB `/probes` volume). Three complementary
fixes: (1) `--max-concurrent-decodes N` CLI flag (default 1) backed by a
`threading.Semaphore` in `bisect_target_vmaf` serialises reference-YUV decodes
across threads; (2) the decoded reference YUV is deleted in the bisect `finally`
block immediately after each (codec, target) pair completes, capping peak disk
to one YUV at a time; (3) a mid-run `shutil.disk_usage` check with 2x headroom
fires before each iteration's decode and returns a structured
`BisectResult(ok=False)` error naming the codec and target VMAF instead of an
opaque ffmpeg rc=228. ADR-0577.
