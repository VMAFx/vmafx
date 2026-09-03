# Revision file (vcs_version.h) generation
vmaf_git_dir = join_paths(libvmaf_src_root + '/../', '.git')

# `--always` is deliberately NOT passed. With it, a checkout that cannot reach
# a `v*.*.*` tag — a shallow CI clone (`actions/checkout` defaults to
# fetch-depth 1, which fetches no tags), a tarball export, or a worktree whose
# `.git` is a file rather than a directory — still exits 0 and prints a bare
# abbreviated commit name. That name then becomes VMAF_VERSION verbatim, so
# `vmaf --version`, the JSON/XML `version` attribute and vmaf_version() all
# report something like "abafdfc" instead of a version. Worse, it is silent
# and intermittent: roughly one commit in a thousand abbreviates to seven hex
# characters that happen to be all letters, which is how it first surfaced
# (merge commit abafdfcc3c… -> "abafdfc", no digit at all).
#
# Without `--always`, git exits non-zero in exactly those cases and meson
# substitutes `fallback` instead — see mesonbuild/scripts/vcstagger.py, which
# catches any exception from the command and uses the fallback string. The
# fallback is spelled out rather than left implicit so the intent survives a
# meson upgrade. When a tag *is* reachable the `--long` form still embeds the
# commit (v3.2.1-42-gabafdfc), so no provenance is lost in the normal case.
rev_target = vcs_tag(command: [
        'git', '--git-dir', vmaf_git_dir,
        'describe', '--tags', '--long',
        '--match', 'v*.*.*'
    ],
    fallback: meson.project_version(),
    input: 'vcs_version.h.in',
    output: 'vcs_version.h'
)

subdir('libvmaf')
