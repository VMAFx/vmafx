Sync `CambiFeatureExtractor` Python wrapper from upstream v0.5 to v0.8,
closing the backlog gap identified in Research-0732 item #4: the
`_validate_asset` guard now fires before any I/O for notyuv assets with a
missing or bitdepth-mismatched `dis_enc_bitdepth`, and `VERSION` tracks the
upstream string used in cached-result directory names.
