- The negative case of the hook-environment test
  (`scripts/githooks/tests/test_install_hooks_env.py`) plants the node-hook
  `npm install` of pre-commit 4.6 itself. pre-commit 4.7.0 runs that install
  without the commit's git variables, so a mutated hook block no longer showed
  the index pollution and the test failed on the framework update.
