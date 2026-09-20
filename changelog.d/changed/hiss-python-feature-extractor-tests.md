- Refactored the Python feature-extractor regression tests into bounded assertion helpers so
  every touched test function satisfies HISS-04 without changing test discovery, fixtures, or
  any Netflix golden assertion, expected value, or tolerance.
