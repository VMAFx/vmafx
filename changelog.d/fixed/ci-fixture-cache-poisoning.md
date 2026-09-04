- Hardened the CI test-fixture cache against poisoning by cancelled or failing runs.
  `actions/cache` saves from a post-job step that runs regardless of outcome,
  and the fixture key is content-hashed over `python/test/*_test.py`, so a run
  cancelled part-way through published a partially-populated
  `python/test/resource` tree under that branch's key. Every later run on the
  branch would then restore the truncated tree and fail with `no frames decoded`,
  because the lazy fetcher only re-downloads fixtures that are *absent*. The
  save is now split out and gated on `success()`, and a restored fixture that
  is empty, or that holds an HTML error page or a Git-LFS pointer instead of
  the payload, is dropped so it re-downloads.
