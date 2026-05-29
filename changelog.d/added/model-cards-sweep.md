Added co-located `_card.md` model cards for all shipped models in `model/`:
- 21 tiny-AI ONNX models under `model/tiny/` (vmaf_tiny_v1–v4, fr_regressor_v1–v3,
  fr_regressor_v2_ensemble_v1, nr_metric_v1, learned_filter_v1, lpips_sq, mobilesal,
  saliency_student_v1, transnet_v2, fastdvdnet_pre, dists_sq, smoke probes)
- 8 upstream Netflix JSON models (`vmaf_v0.6.1`, `vmaf_4k_v0.6.1`, `vmaf_float_v0.6.1`,
  `vmaf_b_v0.6.3`, `vmaf_float_b_v0.6.3`, 4K/neg variants)
- 4 sharded ensemble directories (`vmaf_rb_v0.6.2`, `vmaf_rb_v0.6.3`,
  `vmaf_float_b_v0.6.3`, `vmaf_4k_rb_v0.6.2`)
- `other_models/` group README with legacy model catalogue

Each card covers training data + provenance, hyperparameters, eval metrics
(PLCC / SROCC / RMSE where available), operating point (backend / resolution /
bit depth), known limits, and license + lineage.
