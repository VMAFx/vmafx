- **`vmaf` no longer leaks its option dictionaries when a run stops early.**
  The dictionaries built from `--feature name=opt=val` and from a `--model`
  feature overload (`--model version=...:vif.vif_enhn_gain_limit=1.0`) were
  released only by the libvmaf calls that take them. A run that stopped
  before those calls (an input that cannot be opened, an odd height with
  4:2:0, a model or feature that does not fit the frame, a feature after a
  failing one, an unknown extractor name, a feature pinned to a backend the
  run did not start) exited with them allocated, and LeakSanitizer builds
  failed with a 158 to 329 byte leak. `cli_free()` now
  releases every dictionary the settings still own, and each hand-off to
  libvmaf clears the settings' copy first. Exit codes and output are
  unchanged.
