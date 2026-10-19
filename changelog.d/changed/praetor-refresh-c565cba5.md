- **The praetor pin moves to `c565cba50b57`.** `praetorctl adopt` changes no tracked file. The
  engine's HISS-01 scan now reports one-line `goto` statements; the 33 in the tree, all in the Metal
  extractors under `core/src/feature/metal/`, jump forward to their function's cleanup label, so no
  code changes and no exception is added.
