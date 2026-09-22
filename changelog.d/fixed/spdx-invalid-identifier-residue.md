- **The last live references to a licence that does not exist are gone, and
  one file's licence tag is now machine-readable at all.** ADR-1255 corrected
  every `SPDX-License-Identifier:` declaration that carried the non-existent
  `BSD-3-Clause-Plus-Patent`, and ADR-1250's relicensing finished the job;
  measured on this tree, **no file declares an invalid identifier** and
  `reuse lint` reports only real identifiers in use. What ADR-1255 could not
  reach was everything that states a licence in some *other* syntax, because
  it anchored on the tag. That left 46 files (74 occurrences) naming the phantom licence where
  it still governed behaviour or instructions: the 23 fork-trained entries in
  `model/tiny/registry.json` (whose schema calls the field "SPDX identifier of
  the model's license" and whose sibling `license_url` already pointed at the
  root `LICENSE`), the schema description telling future contributors to use
  it, `api/openapi/vmafx-server-v1.yaml`'s `info.license`, the sidecar
  metadata emitted by two model-export scripts, three test fixtures, every
  tiny-AI model card, and `GOVERNANCE.md` telling contributors they license
  their work under it. Model and data artefacts move to
  `BSD-2-Clause-Patent`, the licence the root `LICENSE` declares and the one
  ADR-1255 chose for exactly this residual class; statements about
  fork-authored code move to `EUPL-1.2`, which `CONTRIBUTING.md` and
  `README.md` already state. Licence-compatibility notes that justified
  rejecting a GPL dependency now cite ADR-0332's ruling instead of naming an
  identifier, so removing the phantom does not silently assert a new
  compatibility claim under EUPL-1.2 — that question belongs to ADR-1250.
  One mention is deliberately left: the module docstring of
  `ai/scripts/train_saliency_student.py`. Correcting that one word would make
  the file "touched", and the repository's touched-file rule then requires its
  pre-existing 148-LOC `main()` to be split — a refactor of an untestable
  training entrypoint (`torch` is not importable here) that a one-word
  docstring fix does not justify.
  `docs/development/cargo-deny.md` also claimed `vmafx-sys` declares
  `BSD-3-Clause` and `vmafx-tad` an identifier cargo-deny cannot parse; both
  in fact inherit the workspace `BSD-2-Clause-Patent`, which `deny.toml`
  already allows. Separately, `scripts/ci/cppcheck-public-entrypoints.cfg`
  put its tag on the same line as its copyright, so REUSE detected **no**
  licence for it at all; split onto its own line, it is now recognised
  (`reuse lint`: 1840 → 1841 files with licence information). The 64 files
  that still name the old string are history — the ADRs, changelog and
  research digests that record the correction, and the repair table in
  `scripts/dev/relicense_fork_files.py` that needs the string to do its job.
