Made `vmaf-tune` cache index and metadata sidecars use the shared strict JSON
writer while preserving cache-key identity; non-finite cached VMAF metadata now
becomes a miss instead of replaying a corrupt cached score.
