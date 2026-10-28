- **Documentation C samples compile, and every exported `vmafx_*` symbol is
  documented (issue 2436).** A C sample marked `<!-- compile -->` is compiled
  against the public headers by the Meson test `test_api_doc_samples`, and
  `test_api_doc_reachability` fails on an exported symbol no user page names
  ([C API samples that compile](docs/development/docs-site-design.md#c-api-samples-that-compile)).
