- The native release-artifact verifier accepts release candidates and the
  version a release build actually reports. It refused `1.0.0-rc.1`, and it
  compared `vmaf --version` with the bare version although a build at the tag
  reports `v1.0.0-rc.1-0-g<commit>`; every release, the final 1.0.0 included,
  would have failed publication. It now accepts exactly the bare version or
  the on-tag `git describe` form.
