## Host C exception list for the pelorus mirror (2026-10-16, ADR-2478)

`.config/lint-exceptions.d/host-c-cpp.toml` lists the ten files of `scripts/ci/pelorus-mirror-paths.txt`. A pelorus re-pin that adds or renames a mirror file updates both lists in the same PR; `test_host_c_list_names_every_pelorus_mirror_file` fails otherwise. no upstream file.
