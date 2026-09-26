- Resolved seven CodeQL `cpp/include-non-header` alerts (908, 943, 955, 1043,
  1203, 1218, 1241) across libvmaf core test translation units. Replaced
  unity-style `.c` and `.cpp` inclusions with internal header declarations and
  link-time seams (`feature/luminance_tools.h`, `feature/feature_name.h`,
  `model.h`, `libvmaf_priv.h`, and `feature/cambi_internal.h`). All white-box
  unit tests and Netflix golden assertions are preserved without exposing private
  APIs publicly or suppressing CodeQL findings.
