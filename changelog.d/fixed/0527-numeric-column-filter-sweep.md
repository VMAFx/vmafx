**ai/scripts/feature_correlation.py**: filter non-numeric columns (e.g. `codec`,
`chug_orientation`) via `select_dtypes(include="number")` before passing to
`to_numpy(dtype=np.float64)`. Parquets from CHUG and KonViD refresh tables carry
string metadata columns that previously caused `ValueError: could not convert string
to float`. Re-applies the fix from commit `0abe9ec462` as part of a sweep across
all scripts in the AI training pipeline that pull feature columns from `df.columns`
dynamically.
