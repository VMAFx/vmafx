### AI

- Migrated `ai/scripts/train_konvid.py`, `ai/scripts/train_konvid_mos_head.py`,
  `ai/scripts/aggregate_corpora.py`, and `ai/train/train.py` to the shared
  `ai/scripts/_script_bootstrap.py` import bootstrap and `aiutils.cli_helpers`
  (`collect_cli_argv`, `make_argument_parser`). Eliminates the last ad hoc
  `sys.path` blocks in the train/aggregate script family; `aggregate_corpora.py`
  gains a previously missing direct-invocation path setup.
