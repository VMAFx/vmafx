- **The documented range of `integer_aim` is corrected: it is not bounded by
  1.** The fixed-point `adm` extractor reports the additive impairment divided
  by the reference's detail as it is, and `float_adm` clips the same ratio at
  1; both follow upstream Netflix/vmaf. On a reference without detail the two
  differ: a flat grey 64x64 reference against the same picture with isolated
  patches gives `integer_aim` 3.1756 (upstream master prints 3.175585) and a
  float `aim` of 1, and with the default model's weight and floor
  `integer_adm3` 0.5 against a float `adm3` of 0.7. No score changes. The
  metrics guide now states both ranges, the definition of `adm3_score` and
  what to expect on such content, and a test pins both behaviours so that
  neither changes unnoticed (ADR-1417,
  `T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01`;
  [features](docs/metrics/features.md)).
