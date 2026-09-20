# Intel NEO fetch hardening

- The development-container Intel NEO resolver now keeps GitHub credentials on
  `api.github.com`, rejects unsafe redirect targets and ambiguous release
  assets, bounds metadata reads, downloads packages atomically with retries,
  and removes corrupt output after checksum or package-validation failures.
