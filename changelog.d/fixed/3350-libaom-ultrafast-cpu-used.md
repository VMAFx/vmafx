- **`libaom-av1` `ultrafast` maps to `-cpu-used 8`, the highest value FFmpeg accepts.**
  The adapter mapped `ultrafast` to 9, which FFmpeg's `libaom-av1` wrapper refuses
  (`Value 9.000000 for parameter 'cpu-used' out of range [0 - 8]`), so every
  `ultrafast` encode and the predictor probe that uses it failed at option
  parsing. The Python adapter, the Go twins (`pkg/encoder` and
  `pkg/codecadapter`) and the docs now agree on 0..8, a unit test pins every
  accepted preset inside that range, and the libaom adapter cache version moves
  to `2` so cached argv from the old mapping is not reused.
