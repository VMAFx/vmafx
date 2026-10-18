## Compat ABI Parity: Upstream Consumer and Plant Gate (ADR-3339)

- `scripts/dev/compat_abi_consumer.c`, `scripts/dev/compat_abi_plant.c`: new fork-authored test harnesses.
- `scripts/dev/upstream_parity.py`: added `--abi` and `--plant` driver flags.
- `scripts/ci/setup-golden-build.sh`: added optional ninja target support.
- No rebase impact on Netflix code: all modified and added files are fork-only test tooling and CI gates.
