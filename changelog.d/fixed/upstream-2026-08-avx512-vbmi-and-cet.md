- **AVX-512 builds no longer require VBMI, and x86 assembly declares CET
  shadow-stack support** (ports of upstream Netflix/vmaf `eb1045795` and
  `f85a85369`). The four AVX-512 build targets passed `-mavx512vbmi`, an
  extension no source in the tree uses: no VBMI intrinsic appears anywhere under
  `core/src/`. Requiring it narrowed the CPUs the AVX-512 objects were valid for
  — Skylake-SP and Cascade Lake implement AVX-512F/DQ/BW/CD/VL but not VBMI — for
  no gain. The flag is dropped from all four targets; the full CPU build and
  132/133 unit tests pass unchanged. Separately, `core/src/ext/x86/x86inc.asm`
  now emits the `.note.gnu.property` section advertising
  `GNU_PROPERTY_X86_FEATURE_1_SHSTK`, so binaries linking the assembled x86
  objects are no longer silently opted out of Intel CET shadow-stack enforcement
  by one unmarked object. Verified with `readelf -n`: the assembled `cpuid.obj`
  reports `x86 feature: SHSTK`.
