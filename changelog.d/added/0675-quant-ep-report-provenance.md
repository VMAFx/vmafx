`ai/scripts/measure_quant_drop_per_ep.py` now records shared
`run_provenance` in `results.json` so CPU/CUDA/OpenVINO PTQ evidence carries
the registry, optional fp32 baselines, hardware tag, argv, and report targets.
