<!-- markdownlint-disable MD060 -->
# ADR-0250: Tiny-AI extractor template — shared scaffolding header

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: `ai`, `dnn`, `refactor`, `dx`, `fork-local`

## Context

Every tiny-AI feature extractor (`feature_lpips.c`, `fastdvdnet_pre.c`, the
in-flight `feature_mobilesal.c` from PR #208, the planned
`feature_transnet_v2.c`) opens a `VmafDnnSession`, resolves an ONNX path
via a feature option with an env-var fallback, and (for the
colour-sensitive ones) converts BT.709 limited-range YUV → RGB with
nearest-neighbour chroma upsampling. The extractors that landed first
copy-pasted ~70 lines of identical setup / option-table / cleanup
plumbing into each new file. PR #208 made the duplication visible at
review time — the mobilesal `yuv8_to_rgb8_planes` body is byte-for-byte
identical to the LPIPS one, including comments.

The fork already enforces NASA/JPL Power-of-10 (rule 1: simple control
flow, rule 9: limited macro use) and SEI CERT C, so the dedup mechanism
must not introduce setjmp/longjmp tricks, recursion, or unbounded
preprocessor wizardry. New extractors should be ~30 LOC of
extractor-specific tensor wiring, not ~150 LOC where 70 % is plumbing.

## Decision

We will introduce `core/src/dnn/tiny_extractor_template.h`, a shared
header that exposes three `static inline` helpers and one
struct-literal-emitting macro:

- `vmaf_tiny_ai_resolve_model_path(name, option, env_var)` — feature-
  option-then-env-var lookup with a single user-facing `no model path`
  log line on failure.
- `vmaf_tiny_ai_open_session(name, path, &out)` — `vmaf_dnn_session_open`
  wrapper with the standard `vmaf_dnn_session_open(<path>) failed: <rc>`
  log line on error.
- `vmaf_tiny_ai_yuv8_to_rgb8_planes(pic, dst_r, dst_g, dst_b)` — BT.709
  limited-range YUV → RGB with nearest-neighbour chroma upsampling
  (bit-exact with the previous per-extractor copies).
- `VMAF_TINY_AI_MODEL_PATH_OPTION(state_t, help_text)` — emits the
  standard `model_path` row of a per-extractor `VmafOption[]` table.

The extractor's `init` / `extract` / `close` lifecycle stays
hand-written per file — the shapes vary too much (single-frame,
ring-buffer, large-window) for a generic lifecycle macro to be
worthwhile. The recipe lives in `docs/ai/extractor-template.md`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **A. Inline helpers + one option-table macro (chosen)** | Power-of-10 friendly (no recursion / no setjmp / bounded macros), clangd jumps directly to source, debugger steps through normally. Each helper is ≤ 25 LOC and trivially auditable. | Doesn't dedup the lifecycle skeleton — that stays per-file. | — |
| B. Codegen (Jinja template emitting a stub `feature_<name>.c`) | Maximum dedup; new extractor = one YAML file. | Adds a Python build dep + an extra meson custom-target step. Generated C is harder to debug (stack traces point at generated lines). Pre-commit and clang-tidy paths get longer. The variation between extractors (LPIPS two-input, FastDVDnet 5-frame ring, TransNet 100-frame window) needs four templates anyway. | Cost > savings for a 4-file population. |
| C. Helper functions exposed via a fnptr table (`VmafTinyAiHooks`) — extractors fill `init_alloc` / `release` / `extract` callbacks, the template orchestrates | Cleanest factoring on paper; every extractor reduces to the hook impls. | Indirect calls hide control flow from the static analysers (CERT MSC22-C-friendly but lints noisily). The varying extract shapes (different tensor names, output shapes, score names emitted) push most of the per-frame logic back into the hook anyway. Frame-window extractors need the hook to stash a ring-buffer slot — the template fights the data. | Power-of-10 rule 9 (limited use of function pointers) + the pattern doesn't actually save LOC. |
| D. Per-extractor variants of the template via feature-flag macros (`VMAF_TINY_AI_WITH_RING_BUFFER`, `VMAF_TINY_AI_WINDOW_SIZE = N`) | Single template handles all four cases. | Macro chain becomes the actual API surface; debugging a misbehaving extractor means debugging the macro expansion. Violates Power-of-10 rule 9 in spirit (heavy preprocessor reliance). | Same `cost > savings` argument as B with worse debuggability. |

## Consequences

- **Positive**: New tiny-AI extractors drop ~70 LOC of plumbing each.
  `feature_lpips.c` shrinks from 305 → ~205 LOC, `fastdvdnet_pre.c` from
  341 → ~317 LOC. Bit-exact behaviour preserved (the YUV→RGB body and
  option-table layout are literal moves). When PR #208 (mobilesal) and
  the TransNet V2 work land, they pick up the helpers in their next
  rebase and shed another ~140 LOC of duplication.
- **Negative**: One additional include in each extractor; clangd needs
  the `libvmaf/src` include path configured (already the case in the
  meson build — the worktree-side IDE warning is cosmetic). The shared
  YUV→RGB helper now sits in `dnn/` instead of `feature/` — slight
  conceptual stretch, justified by it being tiny-AI-specific (not
  shared with non-tiny-AI extractors like ciede).
- **Neutral / follow-ups**: The follow-up PRs that land
  `feature_mobilesal.c` and `feature_transnet_v2.c` are expected to use
  the helpers from day one; their rebase notes track the dependency.
  The template header itself is a no-op at link time (everything is
  `static inline`) so it doesn't widen the public ABI.

## References

- `req` (paraphrased): user requested extracting a tiny-AI extractor
  template + macros to deduplicate the boilerplate across four
  extractors, with new extractors targeting ~30 LOC instead of ~150.
- Related ADRs: [ADR-0041](0041-lpips-sq-extractor.md) (LPIPS surface),
  [ADR-0042](0042-tinyai-docs-required-per-pr.md) (tiny-AI doc bar),
  [ADR-0215](0215-fastdvdnet-pre-filter.md) (FastDVDnet),
  [ADR-0218](0218-mobilesal-saliency-extractor.md) (MobileSal).
- Implementation: `core/src/dnn/tiny_extractor_template.h`,
  `core/src/feature/feature_lpips.c`,
  `core/src/feature/fastdvdnet_pre.c`.
- Recipe doc: [`docs/ai/extractor-template.md`](../ai/extractor-template.md).
