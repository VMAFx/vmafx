<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3534: Preserve executable links in the resolved file's directory

- **Status**: Accepted
- **Date**: 2026-11-11
- **Deciders**: Repository maintainers
- **Tags**: python, security, testing

## Context

The macOS framework interpreter discovers its virtual environment from the executed path. The framework installer distributed by Python.org ships its `bin/` directory as root:admin with mode `0775`. The observed chain starts with a user-owned virtual-environment link in an owned `0755` directory, then follows `/Library/Frameworks/Python.framework/Versions/3.14/bin/python` to `python3.14` in that same framework directory. The second link is user-owned; its directory is root-owned and group-writable. ADR-3532 therefore selects the resolved interpreter, loses the virtual environment, and fails imports of installed packages such as `numpy`. This affects users of that framework installer, beyond automated builds.

The resolved-file launch already relies on the directory containing the resolved executable. Anyone able to replace a link in that directory can also replace the resolved executable there. Preserving such a link adds no directory-replacement exposure beyond the existing resolved-file launch. Link ownership and directory ownership remain independent requirements.

## Decision

Preserve a link when its containing directory has the same `(st_dev, st_ino)` as the resolved file's containing directory, without checking that link directory's write modes or macOS ACL. Keep the link and directory ownership checks for every link. Any metadata lookup failure selects the resolved file. Every other link retains the checks in [ADR-3532](3532-trusted-executable-launch-links.md). Walk links before strict executable resolution to preserve missing-executable diagnostics, then evaluate trust against the resolved path. Platforms without `os.getuid` and platforms other than Linux and macOS retain resolved-file launch. This supersedes ADR-3532's nonsticky group/other-write refusal and macOS ACL rule only for links in the resolved file's own directory.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Same-directory exemption | Preserves framework virtual environments without adding directory-replacement exposure | Requires directory identity checks after executable resolution | Chosen |
| Configure tox or virtualenv to copy interpreters | Avoids the failing symbolic-link chain in automated builds | Hides the defect for framework users and only changes the automated environment | Does not repair executable launch behavior |
| Trust the admin group | Preserves links in group-writable framework directories | Complete group membership cannot be enumerated; administrative command rights depend on configuration | ADR-3532 already rejects membership-based trust |
| Keep the existing rule | Requires no implementation change | Launches the framework interpreter outside the virtual environment and loses its installed packages | Leaves virtual environments broken |

## Consequences

- **Positive**: Framework virtual environments retain the named interpreter path. Tests in `scripts/lib/test_safe_subprocess.py` cover group-write and nonsticky other-write modes, the two-link framework shape, ownership refusal, macOS ACL selection, and metadata failure.
- **Negative**: The resolved executable's directory remains a path-replacement risk shared by named and resolved launches. The exemption does not provide an atomic executable handle.
- **Neutral / follow-ups**: Compare filesystem identities rather than path strings. Keep traversal bounds, resolved-file allowlisting, runnable-file validation, and the byte-identical runner in `tools/rc1-tester/src/vmaf_rc1_tester/_safe_subprocess.py`.

## References

- [ADR-3532: Preserve trusted executable launch links](3532-trusted-executable-launch-links.md).
- CPython `Mac/BuildScript/build-installer.py` (main branch, read 2026-11-11): the framework installer build looks up the `admin` group and adds group write permission to the installed tree, which yields the root:admin `0775` `bin/` directory described above.
- Regression coverage: `scripts/lib/test_safe_subprocess.py`, `python/test/interpreter_launch_test.py`, and `tools/rc1-tester/tests/test_safe_subprocess_copy.py`.
