- **Renovate stops opening duplicate PRs and stops jamming the merge window.**
  Four defects, each observed in the tracker rather than inferred:
  1. **Duplicate PRs for one bump.** The Python group rules matched only the
     `pep621` and `pip_requirements` managers. A dependency that `setup.py`
     also declares is picked up by `pip_setup`, matched no group, and opened a
     second competing PR — #1409 (`renovate/python-(minor)`, from
     `requirements.txt`) and #1410 (`renovate/pywavelets-1.x`, from
     `pyproject.toml` + `setup.py`) are the same PyWavelets bump. `pip_setup`
     and `setup-cfg` are now in those rules. The underlying triplication of the
     dependency list is being removed separately.
  2. **PRs arrived ready-for-review.** With one non-draft PR allowed at a time,
     three bot PRs landing non-draft occupy the window. `draftPR` is now true.
  3. **`prConcurrentLimit: 10` with `prHourlyLimit: 0`** let ten bot PRs open at
     once onto that same one-at-a-time queue. Now 3, with a branch limit of 5.
  4. **No dependency dashboard**, so Renovate's own failures were invisible —
     there was no Renovate issue in the tracker at all. Now enabled.
