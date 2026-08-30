Made `vmaf-roi-score` and `external-bench --out-json` reports strict JSON by
rejecting non-finite ROI scores before output and serializing missing
external-bench aggregate means as `null`.
