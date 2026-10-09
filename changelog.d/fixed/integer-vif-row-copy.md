- **`vif` reads only the samples of each row (Netflix/vmaf `9f4bd165f`).**
  The integer VIF extractor copied a whole picture stride per luma row into
  its buffer. A picture whose stride is wider than its rows and whose last row
  ends before a full stride, such as a wrapped decoder frame or a crop, was
  read past its end. Pictures from `vmaf_picture_alloc()` were not affected;
  no score changes.
