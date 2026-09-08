# Research-2056 — Fedora Dockerfile heredoc repair

## Observed defect

The 2026-09-08 public Scorecard report scanned master
`78c9d2bfc580880919d5168c18ad19441f9db96a` with Scorecard 5.5.0
(`c395761df6afe1a69e476bc60a013a94bcbc153f`). Pinned-Dependencies failed
with `invalid Dockerfile: unterminated heredoc`.

The failing file is `docker/dev/fedora-40.Dockerfile`, byte-identical at that
master and the repair parent `76a7c467c3524478c25d588a31b37b2d45d96081`.
Its optional SYCL instruction starts `<<'EOF'` but the attempted terminator
contains literal `\n` and a continuation. BuildKit never sees a standalone
`EOF` line. This prevents parsing even with the default `ENABLE_SYCL=false`.

Scorecard's pinned BuildKit 0.26.3 parser rejects that file and accepts the
other fourteen tracked Dockerfiles. Docker's installed frontend independently
rejects it before base-image resolution. The root Dockerfile's `done < file`
loop parses correctly: Research-0053's earlier parser-bug hypothesis was
not the cause. Scorecard matches nested `*Dockerfile*` paths too.

## Repair and alternatives

| Option | Consequence | Decision |
| --- | --- | --- |
| Literal-line `printf '%s\n'` with checked redirection | Emits the seven intended repository lines inside the existing conditional | Chosen |
| Correct native Dockerfile heredoc | Can express the same configuration but adds delimiter/frontend syntax to this short branch | Unnecessary |
| Rewrite the root while loop or disable Pinned-Dependencies | Leaves the actual invalid Fedora file in place | Rejected |
| Pin new RPM versions while fixing syntax | Changes distro/SDK resolution without a verified package manifest | Separate work |

The base image, CUDA branch, SYCL default, RPM names/order, signature checks,
Meson arguments and entrypoint are unchanged. The repository file must be
written successfully before installation; installation must succeed before
cleanup. No architectural policy changed, so no new ADR is required.

## Validation

- Actual Scorecard 5.5.0 `validateDockerfilesPinning` and
  `validateDockerfileInsecureDownloads`: original produces the exact error;
  repaired file returns success from both callbacks.
- Exact BuildKit 0.26.3: all fifteen tracked Dockerfiles parse after repair.
- Installed Docker Buildx 0.36.1 with the default Docker driver:
  `--call=targets` rejects the original and accepts the repair.
- ShellCheck 0.11.0 accepts the extracted POSIX shell instruction.
- Four private, network-disabled container cases execute that instruction:
  disabled branch has no output/calls; enabled branch writes exact repository
  bytes then installs/cleans; an install failure stops cleanup; a repository
  write failure stops installation. Package execution is deliberately stubbed.

The syntax-only reproducer requires Docker and does not resolve or build the
base image:

```sh
docker buildx build --builder default --call=targets \
  -f docker/dev/fedora-40.Dockerfile .
```

Hadolint 2.14.0 still reports three pre-existing DL3041 package-version
warnings once the repaired branch is parseable. No tracked Hadolint
configuration or invocation exists in the current Make, pre-commit or CI
gates; standalone Hadolint exits 1 on these warnings. No ignores, threshold
changes or arbitrary RPM pins were added. This is not a complete Fedora/SDK
build, a whole-tree
lint pass, a public Scorecard refresh or RC1 acceptance. The private probe's
Ubuntu-based image tests shell control flow only; it cannot establish Fedora
repository/package availability.

Raw source/download hashes, parser and callback harnesses, before/after
files, commands, logs and branch probes are retained locally under
`.workingdir2/evidence/scorecard-parser-20260908/`. These ignored receipts are
not files shipped to external reviewers; the public reproducer above and
upstream source identify the check independently.

## References

- req: user, “well fix” (Scorecard gaps).
- [Scorecard pinned dependencies implementation](https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/checks/raw/pinned_dependencies.go).
- [Scorecard dependency versions](https://github.com/ossf/scorecard/blob/c395761df6afe1a69e476bc60a013a94bcbc153f/go.mod).
- [BuildKit 0.26.3 parser](https://github.com/moby/buildkit/blob/v0.26.3/frontend/dockerfile/parser/parser.go).
- [Earlier investigation](0053-ossf-scorecard-investigation.md).
- [Fedora development image](../development/fedora-development-image.md).
