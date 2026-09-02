All public C-API functions in `core/include/libvmaf/*.h` now carry Doxygen
`@brief`, `@param`, `@return`, and `@thread-safety` blocks. The standard
thread-safety note is: "Not thread-safe. Use one VmafContext per thread."
`vmaf_version()` is the sole exception (safe to call from any thread).
(ADR-0788, PR #115 follow-up)
