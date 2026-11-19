<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3534: Preserve executable links in the resolved file's directory

- **Status**: Accepted
- **Date**: 2026-11-11
- **Deciders**: Repository maintainers
- **Tags**: python, security, testing

## Context

The macOS framework interpreter discovers its virtual environment from the executed path. The framework installer distributed by Python.org ships its `bin/` directory as root:admin with mode `0775`. The observed chain starts with a user-owned virtual-environment link in an owned `0755` directory, then follows `/Library/Frameworks/Python.framework/Versions/3.14/bin/python` to `python3.14` in that same framework directory. The second link is user-owned; its directory is root-owned and group-writable. ADR-3532 therefore selects the resolved interpreter, loses the virtual environment, and fails imports of installed packages such as `numpy`. This affects users of that framework installer, beyond automated builds.

The resolved-file launch already relies on the directory containing the resolved executable. Directory write access normally permits replacement of both a link and its resolved executable, but the executable's own immutable, append-only, no-unlink, or restricted flags can forbid its replacement while leaving the link replaceable. Preserving the link adds no directory-replacement exposure only when the resolved file is unprotected or the link also has replacement protection. Link ownership and directory ownership remain independent requirements.

## Decision

Preserve a link without checking its directory's write modes or macOS ACL only when its containing directory has the same `(st_dev, st_ino)` as the resolved file's containing directory and the resolved file is known to lack replacement protection or both entries are protected. Keep the link and directory ownership checks for every link. If protection is unknown or only the resolved file is protected, retain the directory-mode and macOS ACL checks from [ADR-3532](3532-trusted-executable-launch-links.md); an owned `0755` directory can still authorize the link. Ownership or directory metadata lookup failures select the resolved file. Every other-directory link retains ADR-3532's checks. Walk links before strict executable resolution to preserve missing-executable diagnostics, then evaluate trust against the resolved path. Platforms without `os.getuid` and platforms other than Linux and macOS retain resolved-file launch. This conditionally supersedes ADR-3532's nonsticky group/other-write refusal and macOS ACL rule only for links in the resolved file's own directory.

Read the resolved file's protection once per launch. On macOS, inspect each entry's own `lstat().st_flags` for `UF_IMMUTABLE`, `UF_APPEND`, `SF_IMMUTABLE`, `SF_APPEND`, `SF_NOUNLINK`, or `SF_RESTRICTED`. On Linux, read the resolved file's `FS_IOC_GETFLAGS` through a read-only, no-follow, nonblocking, close-on-exec descriptor; `FS_IMMUTABLE_FL` and `FS_APPEND_FL` protect the file, while symlinks have no inode protection flags. Compute the generic `_IOR('f', 1, long)` request only for the `uname -m` spellings `x86_64`, `aarch64`, `riscv64`, `armv6l`, `armv7l`, `armv8l`, and `i686`; 32-bit ARM kernels append the endianness letter to the architecture name, so a bare `arm` never appears. An unsupported architecture, big-endian ARM included, leaves protection unknown. The ioctl errors `ENOTTY`, `EOPNOTSUPP`, `ENOSYS`, and `EINVAL` indicate no filesystem flag support and count as unprotected; other read failures leave protection unknown.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Same-directory exemption with replacement protection checks | Preserves framework virtual environments when the entries share replacement exposure | Requires directory identity and file protection checks after executable resolution | Chosen |
| Ignore file flags | Avoids reading entry protections | Allows replacement of an unprotected link to a protected executable | Rejected: reopens the retarget window |
| Configure tox or virtualenv to copy interpreters | Avoids the failing symbolic-link chain in automated builds | Hides the defect for framework users and only changes the automated environment | Does not repair executable launch behavior |
| Trust the admin group | Preserves links in group-writable framework directories | Complete group membership cannot be enumerated; administrative command rights depend on configuration | ADR-3532 already rejects membership-based trust |
| Keep the existing rule | Requires no implementation change | Launches the framework interpreter outside the virtual environment and loses its installed packages | Leaves virtual environments broken |

## Consequences

- **Positive**: Framework virtual environments retain the named interpreter path when file protection permits the exemption or directory permissions protect the link. Tests in `scripts/lib/test_safe_subprocess.py` cover group-write and nonsticky other-write modes, the two-link framework shape, ownership refusal, macOS ACL selection, metadata failure, individual protection flags, ioctl error classification, unsupported architectures, and one resolved-file flag read per launch.
- **Negative**: The resolved executable's directory remains a path-replacement risk shared by named and resolved launches. The exemption does not provide an atomic executable handle.
- **Negative**: Immutable, append-only, no-unlink, or restricted resolved files behind an unprotected link in a group-writable directory launch by resolved path; the virtual-environment prefix is lost in that rare layout.
- **Negative**: Flag read failures fail closed for the exemption. Existing directory trust checks must authorize the named path; otherwise launch uses the resolved file.
- **Neutral / follow-ups**: Compare filesystem identities rather than path strings. Keep traversal bounds, resolved-file allowlisting, runnable-file validation, and the byte-identical runner in `tools/rc1-tester/src/vmaf_rc1_tester/_safe_subprocess.py`.

## References

- [ADR-3532: Preserve trusted executable launch links](3532-trusted-executable-launch-links.md).
- CPython `Mac/BuildScript/build-installer.py` (main branch, read 2026-11-11): the framework installer build looks up the `admin` group and adds group write permission to the installed tree, which yields the root:admin `0775` `bin/` directory described above.
- Regression coverage: `scripts/lib/test_safe_subprocess.py`, `python/test/interpreter_launch_test.py`, and `tools/rc1-tester/tests/test_safe_subprocess_copy.py`.
