- CI: the FFmpeg Windows MSVC leg and the MinGW build leg turn off the
  setup-msys2 package cache. With `path-type: inherit`, the inherited runner
  environment can leave `xargs` less than `LINE_MAX` of MSYS2's `ARG_MAX`; the
  cache pruning (`paccache`) then stops on the findutils assertion
  `bc_ctl.arg_max >= LINE_MAX`, as it did on runner image win25-vs2026
  20261030. `Msys2InheritContract` in
  `scripts/ci/test_ffmpeg_patch_workflow_contract.py` refuses such a step with
  the cache on.
