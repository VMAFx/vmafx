- **Every allocation in the bundled libsvm predictor is checked, and the whole
  file now meets the project's size limit.** A newer cppcheck, which arrived
  with the newer CI runner image, found 88 places where a `malloc` result was
  used without testing it, plus a cache class that owned raw memory while
  allowing itself to be copied. The allocation helper now reports and stops on
  failure, matching what the same file already did for its reallocations, and
  the copy is a compile error rather than a double free. Splitting the file's
  long functions came with that, and every one was split along a seam its own
  comments already marked. Scores are unchanged: all 48 frames of the Netflix
  reference pair are byte-identical at maximum precision, as are both
  checkerboard pairs.
