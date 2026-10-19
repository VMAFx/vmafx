<!-- markdownlint-disable MD013 MD060 -->
# ADR-2073: The provenance record is canonical JSON with one digest over the configuration and the scores, the library writes it into every report, and `--verify-provenance` re-runs it

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (popups 2026-10-06); RC4 work package 5
- **Tags**: api, rc4, provenance, cli, report, server, mcp, abi

## Context

Issue #2142 asks that every score say how it was made: library and ABI
version, build, model and its content hash, the backend and device of every
feature, the options, and a way to re-run a stored score and confirm bit
equality. [ADR-1852](1852-vmafx-api-redesign.md) (design section 2.9) moves
the record into the library so that every consumer gets the same one, and
leaves three choices to this work package: the sidecar for CSV and SUB
reports, how the record is canonicalised and digested, and how a report is
verified. Two later items constrain those choices. Issue #2159 (release 1.1)
signs the record as an assertion of a content-provenance manifest, so the
record needs one byte form a third party can recompute, and a tampered score
must fail verification. VMAFx/pelorus#81 defines a canonical record of the
encode; the score record must have a place for its digest now.

Before this change the record had six fields, the CLI spliced it and the
backend receipt (`backend_used`, `feature_backends`,
[ADR-1359](1359-cli-feature-backend-twin.md)) into the JSON file after the
library had written it, XML, CSV and SUB carried none of it, and a feature was
mapped to its extractor through the extractors' `provided_features`, which
misses option-decorated names such as `mse_y` or `cambi_cmxv_10`.

## Decision

We will make the provenance record a library object with one canonical text
and digest, written into every report by the library:

1. **Record.** `VmafxProvenance` grows at the end (ABI 0.1.5, a patch bump
   under [ADR-1897](1897-vmafx-abi-0x-numbering.md)): commit, `build_id`,
   compiler, build flags, the strict floating-point policy
   ([ADR-1461](1461-strict-fp-every-translation-unit.md)), backends, Rust
   extractors, the SIMD level after the cpumask, the device (index, name,
   runtime), threads, subsampling, masks, the frames (size, layout, depth,
   count), counts of models, features and annotations, the encode-record
   digest, the scores digest, the elapsed time and the record digest. Models
   (name, version or path, SHA-256 of the bytes as loaded, flags, overrides),
   features (feature, extractor, `c` or `rust`, backend, device, runtime,
   options, exactness class, source) and caller annotations are size-prefixed
   structs read by index. The proto `Provenance` message carries them as
   repeated messages numbered from 100 (generator key `proto_repeated`), so
   the field-order numbers of the scalar fields never meet them.
2. **Canonical form and digests.** The record's JSON follows the proto JSON
   mapping of `Provenance` (field names as keys, 64-bit integers as strings,
   enums by their lower-case value names) in the form of RFC 8785: keys in
   byte order, no whitespace, integers in plain decimal, strings escaping only
   `"`, `\` and control characters; invalid UTF-8 becomes U+FFFD. For this
   record (ASCII keys, strings, integers below 2^53) that form equals Python's
   `json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=False)`,
   which the tests use as the independent implementation. `digest` is
   `sha256:` and the SHA-256 of the canonical text without `digest` and
   `elapsed_ns`. `scores_digest`, inside the digested text, is the SHA-256 of
   one line `<report name> <frame> <16 hex digits of the IEEE-754 bits>` per
   score, features in byte order, frames in order, so the one digest binds the
   configuration and every score. #2159 signs that digest (or the canonical
   bytes) and needs no further canonicalisation.
3. **Reports.** The engine's writers embed the record: JSON gets a
   `provenance` object, a `score_format` member and the backend receipt
   (kept as aliases, HISS-14); XML gets one `<provenance>` element with one
   attribute per field and `<model>`, `<feature>` and `<annotation>` children.
   CSV and SUB keep their bytes; `vmafx_report_write()` with
   `VMAFX_REPORT_PROVENANCE_SIDECAR` (`vmaf --provenance-sidecar`) also writes
   `<path>.provenance.json`. The CLI's splice and its receipt formatter are
   removed: one implementation, the library's.
4. **Producers.** The feature collector records the producer of each feature
   vector with its first score: the engine installs the producer (extractor
   instance and options, a model, an import) on the writing thread around
   every extractor call, prediction and import. The exactness class comes from
   a table generated from `scripts/ci/exact_twins.d`, `LIBM_TWINS` and
   `FEATURE_TOLERANCE` (`scripts/codegen/vmafx_exactness.py`), keyed by the
   registered twin name; an alias feature of the gate must share its base's
   exact and libm backend sets, or generation stops.
5. **Verification.** `vmafx_report_open()`, `vmafx_report_field()` and
   `vmafx_report_verify()` read a JSON report back, rebuild the canonical text
   of its record from the tokens and compare a report with a re-run member by
   member, skipping the environment (version, commit, build, device, SIMD
   level, timing, the exactness table), and name the first difference with
   `VMAFX_E_MISMATCH`; then each report's own digests are checked (the scores
   digest only for a lossless `%.17g` report). The CLI records its command
   line without the output options as `cli_argv` annotations; `vmaf
   --verify-provenance <report>` re-runs them into `<report>.rerun.json`,
   exits 0 on a match (removing the re-run), 1 on a difference (keeping it)
   and 2 when the check cannot run.
6. **Encode record.** `vmafx_context_set_encode_record()` takes `sha256:` and
   64 hex digits; the record carries it and the digest covers it, so the
   pelorus encode record (VMAFx/pelorus#81) is bound to the score once a
   producer sets it.
7. **Build id.** `build_id` digests the compilers, build options, FP policy,
   backends and Rust flag; the commit and the architecture have fields of
   their own and are not covered, so two builds of one configuration from
   different commits share an id.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| RFC 8785 subset of the proto JSON mapping (chosen) | One text for the report, the server (protojson reads it without loss) and the 1.1 assertion; recomputable with any JSON library that sorts keys; readable | Floats must stay out of the digested part (none are in it); the writer must keep the mapping | Chosen |
| Deterministic CBOR (RFC 8949 section 4.2) | Compact, binary, defined canonical rules | A second encoding next to the JSON reports; needs a CBOR library in every verifier; not human-readable in a report | Two forms of one record |
| Protobuf deterministic serialisation | Already have the message | Protobuf documents deterministic output as stable only for one binary and version, not across languages or releases | Not a canonical form |
| Digest of the record only, scores separate | Record digest stable while scores accumulate | A signature over the record alone does not cover the scores; #2159 then needs a second digest and a rule binding them | One digest covering both is simpler to sign |
| Sidecar always written | Every format carries provenance | Surprise files next to every CSV; callers that glob output directories break | Opt-in flag instead |
| CSV comment line with the record | One file | CSV readers treat it as a row; breaks the "CSV unchanged" contract | Rejected |
| Verify from the record alone (rebuild options from fields) | No dependence on the CLI | The record does not hold input paths or how frames were read; the re-run would need a second option mapping | The recorded command line re-runs the same code path |
| Producer through a new append argument | Explicit | Every extractor's append call changes (hundreds of call sites, all backends) | Thread-local producer installed by the engine around each call |
| Producer from `provided_features` (prototype) | No engine change | Misses option-decorated names and imported scores | The limitation this work package removes |
| Hand-written repeated messages in `vmafx.proto` | No generator change | Two definitions of the record's shape; the strict server parse would need a split | `proto_repeated` keeps the definition the only source |

## Consequences

- **Positive**: every surface (C API, CLI JSON / XML and the sidecar, the
  scoring server, both MCP servers, the Go binding) carries the same record;
  a report can be checked on its own (digests) or against a re-run; #2159
  signs one digest; the encode record has its slot.
- **Negative**: the JSON report grows by the record (a few kilobytes); the
  collector stores one producer string per feature vector; the report writer
  hashes every score once per report.
- **Neutral / follow-ups**: the device name and runtime of CUDA, SYCL, HIP and
  Metal devices come with the backend lanes of RC4 work package 3
  (`unknown` runtime until then); `implementation` reads the `_rust` suffix
  until the Rust extractor framework (#2086) flags its extractors; the struct
  JSON writer and the exactness table could become emitters of the API
  generator (requests WP1-6, WP1-7); the FFmpeg filters (work package 9) write
  the record through `vmafx_report_write()`.

## References

- `Q` (maintainer popup 2026-10-06, canonical form): "Proto JSON + RFC 8785 (Recommended)".
- `Q` (maintainer popup 2026-10-06, what the digest covers): "Record + scores, not timing (Recommended)".
- `Q` (maintainer popup 2026-10-06, CSV and SUB reports): "Opt-in sidecar (Recommended)".
- `Q` (maintainer popup 2026-10-06, verification): "Re-run and compare (Recommended)".
- `req` (work package brief, 2026-10-06): "provenance covers library + ABI version, build identity (commit, compiler, flags incl. the strict-FP policy), backend + device + driver, model id + SHA-256, options, input description and timing. It is deterministic and canonicalised, hashable for #2159. `--verify-provenance` re-runs or checks a report and fails on a planted mismatch."
- Issue #2142 (provenance on every score), issue #2159 (signed assertion, 1.1), VMAFx/pelorus#81 (encode provenance record).
- [ADR-1852](1852-vmafx-api-redesign.md) design section 2.9; [ADR-1897](1897-vmafx-abi-0x-numbering.md); [ADR-2044](2044-vmafx-option-groups-scoring-contract.md); [ADR-1359](1359-cli-feature-backend-twin.md); [ADR-1461](1461-strict-fp-every-translation-unit.md).
- RFC 8785, JSON Canonicalization Scheme (JCS): <https://www.rfc-editor.org/rfc/rfc8785>.
