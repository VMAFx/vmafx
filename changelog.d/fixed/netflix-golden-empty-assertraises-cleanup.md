- **Netflix CPU Golden Tests (D24) — master-tip regression cleared.**
  PR #276 (ADR-0749 / ADR-0720 ansnr-assertion sunset) deleted the
  `self.assertAlmostEqual(results[1]["VMAF_*_ansnr_*"], ...)` line that
  was inside `with self.assertRaises(KeyError):` blocks, but left the
  empty `with` blocks with only a `pass` statement. `pass` doesn't
  raise `KeyError`, so each block then failed with
  `AssertionError: KeyError not raised`. The Netflix Golden Tests
  (D24) gate has been silently red on every master push since PR #276
  landed, blocking every PR's required CI. Fix: delete the 7 empty
  `with self.assertRaises(KeyError): pass` blocks in
  `python/test/quality_runner_test.py`.
