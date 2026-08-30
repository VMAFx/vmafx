# saliency_student_v2 promoted to production default (ADR-0444)

`saliency_student_v2` is now the production-default saliency model for
the `mobilesal` feature extractor. It replaces `saliency_student_v1`
with a bilinear-resize decoder (IoU 0.7105 vs 0.6558, +8.3 % relative).

The registry CI job (`lint-and-format.yml → registry-validate`) is
confirmed wired and passing.

Two previously undocumented model cards are added:
`docs/ai/models/learned_filter_v1.md` and
`docs/ai/models/nr_metric_v1.md`.
