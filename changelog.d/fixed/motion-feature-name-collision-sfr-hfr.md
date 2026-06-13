Port upstream Netflix/vmaf fix for feature name collision in `integer_motion` when
multiple motion extractors run concurrently (sfr/hfr scenarios). The
`VMAF_integer_feature_motion_sad_score` intermediate feature was appended with a
hardcoded name instead of going through the dict-based name-mangling path, causing
silent collisions. Fix: use `vmaf_feature_collector_append_with_dict` and look up the
(potentially suffixed) sad name from the dict in `flush()`. Source: Netflix/vmaf@8461ae08.
