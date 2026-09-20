- Refactored the Python feature-extractor, asset, and quality-runner regression tests into
  bounded helpers so every touched test function satisfies HISS-04 without changing test
  discovery, fixtures, or any Netflix golden assertion, expected value, or tolerance.
