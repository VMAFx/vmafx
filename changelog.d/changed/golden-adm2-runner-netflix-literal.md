- **Netflix golden assertions: `test_run_adm2_runner` asserts Netflix's literal
  again ([ADR-1828](docs/adr/1828-port-netflix-golden-updates.md), from #2215).**
  `python/test/quality_runner_test.py` expected `0.9345057708333333` for
  `results[0]["ADM2_score"]`, a value the fork wrote in #1244. The line now
  reads `0.9345149030293786` at `places=4`, as Netflix/vmaf has it
  (`python/test/quality_runner_test.py:651` at `700124a4c`). No score changes:
  the fork's CPU build and a build of Netflix's master both print
  `0.9345057916666667`, which passes with either literal. With this line every
  assertion the file shares with upstream carries upstream's value.
