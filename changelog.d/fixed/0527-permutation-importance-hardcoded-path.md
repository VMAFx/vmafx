- `scripts/dev/permutation_importance.py`: replaced the hardcoded
  developer-machine path `REPO = Path("/home/kilian/dev/vmaf")` with
  `REPO = Path(__file__).resolve().parents[2]` so the script locates
  the repository root correctly on any host
  (T-PYTHON-PERMUTATION-IMPORTANCE-HARDCODED-PATH).
