- **The RC1 tester runs its commands through the repository's bounded runner.**
  `vmaf_rc1_tester.safe_process.run_bounded()` keeps its signature, exceptions
  and messages, and now calls a byte copy of `scripts/lib/safe_subprocess.py`
  shipped in the wheel instead of its own second implementation;
  `tools/rc1-tester/tests/test_safe_subprocess_copy.py` fails when the copy
  and the source differ.
