Fixed `vmaf-tune recommend --from-corpus` so the CLI path applies the
same filtering as the library API: failed rows, non-finite VMAF rows,
and non-matching encoder / preset rows are ignored before picking a
recommendation.
