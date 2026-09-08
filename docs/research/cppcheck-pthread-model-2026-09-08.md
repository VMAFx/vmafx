# Cppcheck pthread model correction (2026-09-08)

## Evidence and mechanism

The configured CPU run reported 16 `uninitMemberVarNoCtor` warnings in
`core/src/model.h` and eight in `core/src/feature/feature_collector.h`.
A minimal C++ translation unit includes those actual headers and explicitly
zero-initializes both aggregates. Cppcheck 2.21.1 still emits all 24 warnings
with its default model, then emits none when only `--library=posix` is added.
The same compile-database control in C is clean in both configurations.

The installed `posix.cfg` byte-matches the official 2.21.1 file. It declares
`pthread_mutex_t` as a POD type and models pthread function arguments, including
`pthread_mutex_init`'s output parameter. Cppcheck's constructor analysis treats
an unknown class-like member as initialized; on C++14 and later that can make
it require initialization of the containing type's ordinary scalar members.
Loading the accurate pthread type model removes that mistaken premise. This is
an analyzer configuration defect, not evidence that these C structs need C++
constructors or 24 member suppressions.

Primary, versioned sources:

- [Cppcheck 2.21.1 POSIX model](https://github.com/danmar/cppcheck/blob/2.21.1/cfg/posix.cfg#L6355-L6367)
- [Cppcheck 2.21.1 constructor analysis](https://github.com/danmar/cppcheck/blob/2.21.1/lib/checkclass.cpp)
- [Cppcheck 2.13.0 POSIX model](https://github.com/danmar/cppcheck/blob/2.13.0/cfg/posix.cfg)

The installed `--help` documents library models separately from platform and
language selection. The model already exists in the older 2.13.0 source used
by distro toolchains; no vendored replacement or new native dependency is added.

## Durable correction

Both `scripts/ci/lint-configured.py` and the required Cppcheck workflow load
`--library=posix`. Their diagnostic categories, failure exit code, suppression
list and compile database remain unchanged. This models an API the fork uses
on every platform: `core/src/compat/win32/pthread.h` implements the same mutex
and condition types on Windows. No Unix platform, C-only language or target
architecture is forced; command variants and Windows defines remain intact.

No new ADR is needed: this supplies missing standard API knowledge to the
existing analyzer contract. No alternatives: per-member suppression would hide
the false premise, and constructors would change a shared C surface for the
benefit of a misconfigured analyzer. No production or header changes are needed.

## Positive and negative controls

The mandatory real-tool test uses the production argv builder and a generated
one-TU compile database with actual repository headers. It runs after cppcheck
installation in the required Cppcheck job; missing tools or models fail.

- Zero-initialized real C aggregates pass in both C and C++.
- Reading `VmafModel.n_features` without initializing its object still produces
  `uninitvar` in C and C++.
- A real C++ constructor that leaves a scalar member unset still produces
  `uninitMemberVar`.
- A real `std::string`-containing class with an unset scalar still produces
  uninitialized-use diagnostics. On versions that implement
  `uninitMemberVarNoCtor`, that exact category also remains active.

The configured-driver fixtures independently require the model flag, unchanged
severity/error flags, preserved C++ and Windows-target command arguments,
unchanged native database bytes and the workflow's real-tool registration.
This does not make cppcheck acceptance equivalent to Windows runtime validation.

Raw versioned sources, compiler controls, analyzer XML and command/result hashes
are retained under `.workingdir2/evidence/cppcheck-c-header-model-20260908`.
The focused controls do not claim whole-tree `make lint`, `make test` or release
acceptance. No clang-tidy baseline change is appropriate for this configuration
fix; no native source was changed or newly measured for that ratchet.
