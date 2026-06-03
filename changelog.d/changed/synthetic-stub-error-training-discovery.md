- `scripts/dev/training_discovery_report.py`: replace silent `synthetic` tagging with an
  explicit `ValueError` when a predictor card contains the `synthetic-stub model` marker.
  Synthetic stubs are no longer a valid in-tree artefact; the error directs the operator
  to train against a real corpus and regenerate the card.
