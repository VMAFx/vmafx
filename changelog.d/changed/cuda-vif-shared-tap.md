- The CUDA `integer_vif` horizontal filter updates its main and reduced-filter
  accumulators through shared inline helpers in `filter1d.cu`, deduping centre
  and border tap logic (HISS-19); scores and compiled SASS are unchanged.
