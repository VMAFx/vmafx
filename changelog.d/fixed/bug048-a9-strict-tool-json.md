Restored RFC-8259 strict report output for `external-bench --out-json` and
`vmaf-roi-score`: non-finite aggregate values serialize as `null`, while an
invalid ROI pooled score exits 65 without writing a report.
