Routed operator-facing `vmaf-tune` CLI JSON outputs through the strict JSON
helper so non-finite diagnostics appear as `null` instead of `NaN` tokens.
