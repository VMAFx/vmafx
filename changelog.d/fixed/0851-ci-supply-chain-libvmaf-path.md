Fix stale `build/libvmaf/libvmaf.so*` artifact path in `supply-chain.yml`; correct
post-ADR-0700 path is `build/src/libvmaf.so*`. Without this fix the SLSA release job
would fail at artifact staging on the first release tag after the `libvmaf/` → `core/`
rename. (ADR-0851)
