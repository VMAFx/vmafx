## Research: CompileIQ pilot on `filter1d.cu` (abandoned — toolchain blockers)

Investigated NVIDIA CompileIQ v1.0.0 as an auto-tuner for the VIF CUDA hot-path
kernel (`core/src/feature/cuda/integer_vif/filter1d.cu`, 845 LOC). Pilot abandoned
before the search ran due to two hard blockers: (1) the devcontainer ships Python 3.14
but CompileIQ 1.0.0 requires `<3.14`; (2) the only published CompileIQ search space
targets CUDA 13.3 while the container contains CUDA 13.2. Re-run pre-conditions and
methodology documented in Research-0734 and ADR-0739.
