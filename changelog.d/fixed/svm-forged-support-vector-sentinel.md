- **Heap buffer overflow loading a model whose support-vector data
  forges the end-of-vector sentinel.** `SVMModelParser::parse_support_vectors()`
  read each feature index straight from the file with no validation.
  libsvm indices are 1-based and strictly positive, and `index == -1`
  is the reserved terminator — so a line like `1.0-1:1.0` parses the
  coefficient as `1.0` and the index as `-1`, splitting one support
  vector into two sentinel-terminated runs. The pointer array is
  `Malloc(svm_node *, model->l)`, sized by the declared `total_sv`,
  while the fill loop writes one entry per run: with `total_sv 1` and
  two runs it writes 8 bytes past a one-element array. Reachable from
  any model JSON, including one fetched over the network. The parser
  now rejects a non-positive index, which makes the sentinel
  unforgeable, and the fill loop additionally bounds itself against
  `total_sv` and requires the two counts to agree — so a future
  divergence is a clean parse error rather than a heap overflow. Found
  by libFuzzer + ASan; reproducer promoted to
  `core/test/fuzz/json_model_corpus/svm_forged_sv_sentinel.bin`.
