# Research digest 1101 — BRISQUE no-reference CPU extractor

Supports [ADR-1115](../adr/1115-brisque-nr-metric.md). Captures the constant
verification, the oracle construction, and the numerical-stability finding that
shaped the test design. Companion to the verified dossier
`.workingdir2/rc/metrics/brisque.md`.

## Question

How do we add a correct, canonical BRISQUE no-reference extractor to the fork,
given that (a) there is no in-tree model, (b) the only runnable C++ reference
(krshrimali) diverges from the pipeline that *trained* the model, and (c) the
reference repo ships two conflicting feature-range files?

## Sources consulted

- Mittal, Moorthy, Bovik, IEEE TIP 2012 (the paper) — Eq.1-3 (MSCN/Gaussian),
  Eq.7-10 (paired products), Eq.12-15 (AGGD/eta), Table I (GGD for the MSCN
  field).
- gregfreeman/image_quality_toolbox `+brisque` — the MATLAB pipeline that
  **trained** the shipped `allmodel` (`brisque_feature.m`, `estimateggdparam.m`,
  `estimateaggdparam.m`, `allmodel`, `allrange`).
- krshrimali/No-Reference-...-BRISQUE-Model — C++ port (`brisque.cpp` AGGDfit,
  `computescore.cpp` scaling+predict inline arrays, `C++/allmodel`,
  `C++/allrange`).
- libsvm 3.x source (`svm_predict` vs `svm_predict_probability` for EPSILON_SVR).

## Findings

### Constants (all independently confirmed)

| Constant | Value | Source agreement |
| --- | --- | --- |
| Gaussian window | 7×7, `sigma = 7/6`, unit-volume | paper Eq.2 + MATLAB `fspecial` + OpenCV + pyiqa (krshrimali's `1.166` is a truncation — rejected) |
| MSCN | `(I−mu)/(sigma+1)` on [0,255] luma, C=1 | paper Eq.1 + MATLAB |
| Paired-product shifts | `(0,1),(1,0),(1,1),(-1,1)` | krshrimali + ocampor + pyiqa + paper |
| Gamma grid | `0.2:0.001:10` inclusive (9801) | MATLAB + krshrimali + pyiqa |
| GGD ratio | `Γ(1/g)Γ(3/g)/Γ(2/g)²` | `estimateggdparam.m` |
| AGGD ratio | `Γ(2/g)²/(Γ(1/g)Γ(3/g))` | `estimateaggdparam.m` |
| f1/f2 fit | **GGD** on the MSCN field | MATLAB `brisque_feature.m` + paper Table I (krshrimali uses AGGD — a bug) |
| AGGD sign buckets | strict `x<0` / `x>0`, **zeros excluded** | `estimateaggdparam.m` (`vec(vec<0)`, `vec(vec>0)`) |
| Output clamp | none | krshrimali path (OpenCV clamps [0,100] — a different model) |
| Predict | `svm_predict` == `svm_predict_probability` for EPSILON_SVR | libsvm source + measured (Δ < 6e-13) |

### Range-array provenance (the dossier's verifier correction)

The `min_[36]`/`max_[36]` arrays come from the **inline** arrays in
`computescore.cpp` (first-5 min `0.336999, 0.019667, 0.230000, -0.125959,
0.000167`), confirmed transcribed exactly. The repo's separate `allrange` file
disagrees on all 36 rows (row1 `0.338/10`, row19 `0.471/3.264`, …) and is never
read by the prediction code. Using `allrange` would corrupt every score. The
inline arrays are the source of truth — the dossier's earlier "verify against
allrange" instruction was inverted and is explicitly NOT followed.

### Model

`allmodel` header: `epsilon_svr`, `rbf`, `gamma 0.05`, `nr_class 2`,
`total_sv 770`, `rho -155.845`, `probA 6.34795` — read directly from the file,
matching the dossier. Vendored as `model/other_models/brisque_live.model`,
sha256 `19526fb799c4c7992ccc109fcfecddb25976ba024b194cd3ee275d27e8909c8d`.

### Oracle construction

There is no in-tree runnable reference and no published pooled score for our
fixtures, so a MATLAB-faithful Python oracle was built: a numpy port of
`estimateggdparam`/`estimateaggdparam`, a numpy port of MATLAB `imresize`
antialiased bicubic, and prediction through the **same** vendored `allmodel` via
the official `libsvm` wheel. Validated on standard natural images, giving
sensible BRISQUE values (cameraman ≈ −13.7, astronaut ≈ 2.9, coins ≈ −5.1,
moon ≈ 1.4) with healthy GGD alphas (1.4–2.8). This is the correctness anchor.

### Numerical-stability finding (shaped the test design)

The AGGD strict-sign bucket (`x<0` / `x>0`, zeros excluded) is the crux. On
**stable natural content** the C extractor matches the oracle tightly:
cameraman C = `-13.708444` vs oracle = `-13.708399` (~5e-5, places=4). But on
**heavily-compressed near-flat content** (the VMAF test frames, whose MSCN field
is nearly flat), a large fraction of paired products land within ~1e-11 of zero;
the sign classification — and hence `eta` — flips with the last bits of the MSCN
field. Two equivalent filter implementations (2D `filter2` vs separable, equal
to 1e-11) produced final scores of 97.5 vs 80.9 on such a frame purely from
sign-classification differences. This is inherent to BRISQUE (the dossier
flagged the same near-zero cliff for NIQE's float32 round-trip).

Consequence for testing: the unit oracles pin the math exactly; the natural-image
cross-check pins end-to-end correctness at places=4; and the in-tree fixture
(frame 12 of `dis_576x324_48f.yuv`, a compressed frame) is **snapshotted** as the
extractor's own deterministic scalar-summation output (`81.066729887948`) rather
than cross-asserted at places=4, because no two implementations agree to 1e-4 on
near-flat content.

## Decision impact

- Follow the MATLAB-trained pipeline (GGD-for-MSCN, sigma 7/6, MATLAB bicubic),
  not krshrimali's C++.
- Use the inline range arrays; never substitute `allrange`.
- No output clamp.
- Snapshot the compressed-fixture score; gate correctness via unit oracles +
  the natural-image cross-check.
- Bundle the model under a documented research-use exception (the maintainer's
  call; see ADR-1115 alternatives).
