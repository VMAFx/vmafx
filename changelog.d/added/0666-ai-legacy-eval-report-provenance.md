Legacy AI evaluation reports now include shared `run_provenance` metadata:
`eval_loso_mlp_small.py`, `eval_loso_3arch.py`,
`eval_probabilistic_proxy.py`, and `eval_saliency_per_mb.py` record their
entrypoint, argv, parsed arguments, named inputs, and report output targets in
durable JSON artifacts.
