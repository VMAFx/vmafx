- **Unmodified upstream consumer ABI drop-in parity verification (ADR-3339).**
  `scripts/dev/compat_abi_consumer.c` compiles against Netflix public headers and
  dynamically links `libvmaf.so.3` to prove that swapping the library at runtime
  produces identical scores. The guard includes an LD_PRELOAD plant shim
  (`scripts/dev/compat_abi_plant.c`) that perturbs pooled scores by 1 ULP to verify
  regression detection.
