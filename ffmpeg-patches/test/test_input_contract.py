"""Red-cap tests for the public FFmpeg input-contract checker CLI."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command


class InputContractCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repo = Path(__file__).resolve().parents[2]
        cls.checker = cls.repo / "ffmpeg-patches/test/check-input-contract.sh"

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "ffmpeg-patches").mkdir()
        (self.root / "docs/usage").mkdir(parents=True)
        (self.root / ".github/workflows").mkdir(parents=True)
        self.write_baseline()

    def write_baseline(self) -> None:
        (self.root / "ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch").write_text(
            """diff --git a/libavfilter/vf_libvmaf.c b/libavfilter/vf_libvmaf.c
--- a/libavfilter/vf_libvmaf.c
+++ b/libavfilter/vf_libvmaf.c
@@ -1,1 +1,4 @@
+    av_log(ctx, AV_LOG_INFO,
+           \"libvmaf: input[0]=distorted input[1]=reference \"
+           \"(opposite of Python runner / vmaf CLI order)\\n\");
""",
            encoding="utf-8",
        )
        (self.root / "Makefile").write_text(
            """.PHONY: ffmpeg-input-contract
ffmpeg-input-contract:
\tbash ffmpeg-patches/test/check-input-contract.sh
\tpython3 -m unittest discover -s ffmpeg-patches/test -p 'test_input_contract.py' -v

lint-sh: ffmpeg-input-contract
""",
            encoding="utf-8",
        )
        workflow_call = (
            "      - name: Enforce input contract\n        run: make ffmpeg-input-contract\n"
        )
        (self.root / ".github/workflows/ffmpeg-patch-stack.yml").write_text(
            f"jobs:\n  check:\n    steps:\n{workflow_call}  refresh:\n    steps:\n{workflow_call}",
            encoding="utf-8",
        )
        (self.root / ".pre-commit-config.yaml").write_text(
            """repos:
  - repo: local
    hooks:
      - id: ffmpeg-input-contract
        name: FFmpeg input contract
        entry: make ffmpeg-input-contract
        language: system
        pass_filenames: false
""",
            encoding="utf-8",
        )
        (self.root / "docs/usage/ffmpeg.md").write_text(
            """# Fixture

```bash
ffmpeg -hide_banner \\
  -hwaccel cuda -i distorted.mp4 \\
  -thread_queue_size 8 -i reference.mp4 \\
  -filter_complex "[0:v][1:v]libvmaf_cuda=log_fmt=json" \\
  -f null -
```

```bash
ffmpeg -i reference.mp4 -i distorted.mp4 \\
  -filter_complex '[0:v]setpts=PTS-STARTPTS[reference];
                   [1:v]setpts=PTS-STARTPTS[distorted];
                   [distorted][reference]libvmaf' \\
  -f null -
```

Wrong, for explanation only:

<!-- vmafx-ffmpeg-input-order: intentionally-wrong -->
```bash
ffmpeg -i reference.mp4 -i distorted.mp4 \\
  -filter_complex "[0:v][1:v]libvmaf" -f null -
```
""",
            encoding="utf-8",
        )
        (self.root / "docs/usage/docker.md").write_text(
            """# Docker Fixtures

```bash
docker run --rm -v $(pwd):/files vmaf \\
    -i /files/distorted.y4m \\
    -i /files/reference.y4m \\
    -lavfi libvmaf \\
    -f null -
```

```bash
docker run --gpus all --rm -v $(pwd):/files vmaf \\
    -i /files/distorted.y4m \\
    -i /files/reference.y4m \\
    -lavfi "[0:v][1:v]libvmaf_cuda" \\
    -f null -
```

```bash
docker run --gpus all -e NVIDIA_DRIVER_CAPABILITIES=compute,video \\
    -v $(pwd):/files ffmpeg_vmaf \\
    -hwaccel cuda -hwaccel_output_format cuda \\
    -i /files/Beauty_3840x2160_120fps_420_8bit_HEVC_RAW.hevc \\
    -hwaccel cuda -hwaccel_output_format cuda -i /files/dist.mp4 \\
    -filter_complex "[0:v]scale_cuda=format=yuv420p[ref];[1:v]scale_cuda=format=yuv420p[dist];[dist][ref]libvmaf_cuda" \\
    -f null -
```
""",
            encoding="utf-8",
        )

    def run_checker(self, root: Path | None = None) -> subprocess.CompletedProcess[str]:
        bash = shutil.which("bash")
        self.assertIsNotNone(bash)
        assert bash is not None
        return run_command(
            [bash, str(self.checker), "--root", str(root or self.root)],
            allowed_executables=(bash,),
            check=False,
            capture_output=True,
            text=True,
            timeout_seconds=30,
        )

    def assert_fails_with(self, text: str) -> None:
        result = self.run_checker()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(text, result.stderr)

    def mutate(self, relative: str, old: str, new: str, count: int = -1) -> None:
        path = self.root / relative
        original = path.read_text(encoding="utf-8")
        self.assertIn(old, original)
        path.write_text(original.replace(old, new, count), encoding="utf-8")

    def test_multiline_options_and_labeled_graph_pass(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_repository_contract_passes(self) -> None:
        result = self.run_checker(self.repo)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_log_call_removal_fails(self) -> None:
        self.mutate(
            "ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch",
            "av_log(ctx, AV_LOG_INFO,",
            "av_log(ctx, AV_LOG_WARNING,",
        )
        self.assert_fails_with("executable AV_LOG_INFO")

    def test_log_message_mutation_fails(self) -> None:
        self.mutate(
            "ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch",
            "input[1]=reference",
            "input[1]=source",
        )
        self.assert_fails_with("executable AV_LOG_INFO")

    def test_commented_log_call_fails(self) -> None:
        call = (
            "+    av_log(ctx, AV_LOG_INFO,\n"
            '+           "libvmaf: input[0]=distorted input[1]=reference "\n'
            '+           "(opposite of Python runner / vmaf CLI order)\\n");'
        )
        self.mutate(
            "ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch",
            call,
            "+    /* disabled reminder\n" + call + "\n+     */",
        )
        self.assert_fails_with("executable AV_LOG_INFO")

    def test_multiline_options_reversal_fails(self) -> None:
        self.mutate(
            "docs/usage/ffmpeg.md",
            "-hwaccel cuda -i distorted.mp4 \\",
            "-hwaccel cuda -i reference.mp4 \\",
        )
        self.mutate(
            "docs/usage/ffmpeg.md",
            "-thread_queue_size 8 -i reference.mp4 \\",
            "-thread_queue_size 8 -i distorted.mp4 \\",
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_labeled_graph_reversal_fails(self) -> None:
        self.mutate(
            "docs/usage/ffmpeg.md",
            "[distorted][reference]libvmaf'",
            "[reference][distorted]libvmaf'",
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_label_source_conflict_fails(self) -> None:
        self.mutate(
            "docs/usage/ffmpeg.md",
            "[0:v]setpts=PTS-STARTPTS[reference]",
            "[0:v]setpts=PTS-STARTPTS[main]",
        )
        self.mutate(
            "docs/usage/ffmpeg.md",
            "[distorted][reference]libvmaf'",
            "[distorted][main]libvmaf'",
        )
        self.assert_fails_with("[main] says distorted but traces to a reference input")

    def test_ambiguous_roles_fail(self) -> None:
        self.mutate("docs/usage/ffmpeg.md", "distorted.mp4", "first.mp4", 1)
        self.mutate("docs/usage/ffmpeg.md", "reference.mp4", "second.mp4", 1)
        self.assert_fails_with("ambiguous VMAF input roles")

    def test_intentional_wrong_marker_removal_fails(self) -> None:
        self.mutate("docs/usage/ffmpeg.md", WRONG_MARKER, "")
        self.assert_fails_with("reversed VMAF pads")

    def test_intentional_wrong_marker_on_correct_example_fails(self) -> None:
        wrong = (
            "ffmpeg -i reference.mp4 -i distorted.mp4 \\\n"
            '  -filter_complex "[0:v][1:v]libvmaf" -f null -'
        )
        correct = (
            "ffmpeg -i distorted.mp4 -i reference.mp4 \\\n"
            '  -filter_complex "[0:v][1:v]libvmaf" -f null -'
        )
        self.mutate("docs/usage/ffmpeg.md", wrong, correct)
        self.assert_fails_with("must demonstrate reference on pad 0")

    def test_workflow_wiring_removal_fails(self) -> None:
        self.mutate(
            ".github/workflows/ffmpeg-patch-stack.yml",
            "make ffmpeg-input-contract",
            "echo contract-removed",
            1,
        )
        self.assert_fails_with("workflow job check must run")

    def test_workflow_call_cannot_be_duplicated_into_one_job(self) -> None:
        self.mutate(
            ".github/workflows/ffmpeg-patch-stack.yml",
            "make ffmpeg-input-contract",
            "echo contract-removed",
            1,
        )
        self.mutate(
            ".github/workflows/ffmpeg-patch-stack.yml",
            "  refresh:\n    steps:\n",
            "  refresh:\n    steps:\n"
            "      - name: Duplicate input contract\n"
            "        run: make ffmpeg-input-contract\n",
        )
        self.assert_fails_with("workflow job check must run")

    def test_precommit_wiring_removal_fails(self) -> None:
        self.mutate(
            ".pre-commit-config.yaml",
            "entry: make ffmpeg-input-contract",
            "entry: echo contract-removed",
        )
        self.assert_fails_with("pre-commit must run")

    def test_make_test_wiring_removal_fails(self) -> None:
        self.mutate(
            "Makefile",
            "python3 -m unittest discover -s ffmpeg-patches/test " "-p 'test_input_contract.py' -v",
            "true",
        )
        self.assert_fails_with("ffmpeg-input-contract target is incomplete")

    def test_lint_wiring_removal_fails(self) -> None:
        self.mutate("Makefile", "lint-sh: ffmpeg-input-contract", "lint-sh:")
        self.assert_fails_with("lint-sh must depend on ffmpeg-input-contract")

    def test_docker_run_positive_passes(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_plain_docker_vmaf_reversal_fails(self) -> None:
        self.mutate(
            "docs/usage/docker.md",
            "-i /files/distorted.y4m \\\n    -i /files/reference.y4m \\\n    -lavfi libvmaf",
            "-i /files/reference.y4m \\\n    -i /files/distorted.y4m \\\n    -lavfi libvmaf",
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_cuda_docker_vmaf_reversal_fails(self) -> None:
        self.mutate(
            "docs/usage/docker.md",
            '-i /files/distorted.y4m \\\n    -i /files/reference.y4m \\\n    -lavfi "[0:v][1:v]libvmaf_cuda"',
            '-i /files/reference.y4m \\\n    -i /files/distorted.y4m \\\n    -lavfi "[0:v][1:v]libvmaf_cuda"',
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_ffmpeg_vmaf_named_graph_reversal_fails(self) -> None:
        self.mutate(
            "docs/usage/docker.md",
            "[dist][ref]libvmaf_cuda",
            "[ref][dist]libvmaf_cuda",
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_docker_run_ambiguity_fails(self) -> None:
        self.mutate("docs/usage/docker.md", "distorted.y4m", "video1.y4m", 1)
        self.mutate("docs/usage/docker.md", "reference.y4m", "video2.y4m", 1)
        self.assert_fails_with("ambiguous VMAF input roles")

    def test_podman_reversal_fails(self) -> None:
        (self.root / "docs/usage/docker.md").write_text(
            """# Podman Reversal

```bash
podman run --rm -v $(pwd):/files vmaf \\
    -i /files/reference.y4m \\
    -i /files/distorted.y4m \\
    -lavfi libvmaf \\
    -f null -
```
""",
            encoding="utf-8",
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_registry_port_qualified_image(self) -> None:
        (self.root / "docs/usage/docker.md").write_text(
            """# Registry Port Qualified

```bash
docker run --rm localhost:5000/vmaf:latest \\
    -i /files/distorted.y4m \\
    -i /files/reference.y4m \\
    -lavfi libvmaf \\
    -f null -
```
""",
            encoding="utf-8",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)

        self.mutate(
            "docs/usage/docker.md",
            "-i /files/distorted.y4m \\\n    -i /files/reference.y4m",
            "-i /files/reference.y4m \\\n    -i /files/distorted.y4m",
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_name_vmaf_with_arbitrary_image_ignored(self) -> None:
        (self.root / "docs/usage/docker.md").write_text(
            """# Arbitrary image with --name vmaf

```bash
docker run --rm --name vmaf ubuntu:22.04 \\
    -i /files/reference.y4m \\
    -i /files/distorted.y4m \\
    -lavfi libvmaf \\
    -f null -
```
""",
            encoding="utf-8",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_ffmpeg_on_arbitrary_image(self) -> None:
        (self.root / "docs/usage/docker.md").write_text(
            """# Explicit entrypoint on arbitrary image

```bash
docker run --rm --entrypoint ffmpeg ubuntu:22.04 \\
    -i /files/distorted.y4m \\
    -i /files/reference.y4m \\
    -lavfi libvmaf \\
    -f null -
```
""",
            encoding="utf-8",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)

        self.mutate(
            "docs/usage/docker.md",
            "-i /files/distorted.y4m \\\n    -i /files/reference.y4m",
            "-i /files/reference.y4m \\\n    -i /files/distorted.y4m",
        )
        self.assert_fails_with("reversed VMAF pads")

    def test_non_ffmpeg_entrypoint_override_rejected(self) -> None:
        (self.root / "docs/usage/docker.md").write_text(
            """# Non-FFmpeg entrypoint override

```bash
docker run --rm --entrypoint /bin/sh vmaf \\
    -i /files/distorted.y4m \\
    -i /files/reference.y4m \\
    -lavfi libvmaf \\
    -f null -
```
""",
            encoding="utf-8",
        )
        self.assert_fails_with("unsupported container entrypoint '/bin/sh'; expected ffmpeg")

    def test_unknown_container_option_error(self) -> None:
        (self.root / "docs/usage/docker.md").write_text(
            """# Unknown container option

```bash
docker run --rm --unsupported-flag vmaf \\
    -i /files/distorted.y4m \\
    -i /files/reference.y4m \\
    -lavfi libvmaf \\
    -f null -
```
""",
            encoding="utf-8",
        )
        self.assert_fails_with("unrecognized or malformed container option '--unsupported-flag'")

    def test_missing_option_arguments_error(self) -> None:
        (self.root / "docs/usage/docker.md").write_text(
            """# Missing container option value

```bash
docker run --rm --gpus= vmaf \\
    -i /files/distorted.y4m \\
    -i /files/reference.y4m \\
    -lavfi libvmaf \\
    -f null -
```
""",
            encoding="utf-8",
        )
        self.assert_fails_with("container option '--gpus' requires an argument")

        self.mutate("docs/usage/docker.md", "--gpus=", "--name=")
        self.assert_fails_with("container option '--name' requires an argument")

        (self.root / "docs/usage/docker.md").write_text(
            """# Missing container option value at end

```bash
docker run --rm -v \\
    -lavfi libvmaf
```
""",
            encoding="utf-8",
        )
        self.assert_fails_with("container option '-v' requires an argument")


WRONG_MARKER = "<!-- vmafx-ffmpeg-input-order: intentionally-wrong -->"


if __name__ == "__main__":
    unittest.main()
