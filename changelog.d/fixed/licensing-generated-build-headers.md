- **Container images build again.** Every image that builds libvmaf stopped
  at its licence scan because two headers the build generates
  (`vmafx_build_info.h`, `vmafx_build_commit.h`) had no entry in the licence
  manifest. Both are listed now, and a test fails when a new generated header
  lacks one.
