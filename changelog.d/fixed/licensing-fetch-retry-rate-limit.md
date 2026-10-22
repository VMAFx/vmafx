- Container images: the licence-text fetch of the release images
  (`tools/rc1-tester/image/licensing.py`) asks a host again after an HTTP 429
  or a 502/503/504 answer, up to five attempts, waiting the host's
  `Retry-After` (at most 60 s) or 2, 4, 8 s. A rate limit from a text host no
  longer fails the AMD GPU image build; any other HTTP error still fails at
  once.
