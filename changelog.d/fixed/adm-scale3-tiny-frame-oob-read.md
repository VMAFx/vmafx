- **`integer_adm_scale3` is now correct and reproducible on frames 17 to 32
  pixels high or wide.** For those sizes the fourth ADM DWT level has only two
  output samples per row. The mirror-index table then replaced its first entry
  with index -1, so scale 3 read one row before its input band and one int32
  before its scratch allocation, which AddressSanitizer reports as a heap
  overflow. The score depended on whatever those bytes held and could change
  between runs on identical input. The table now keeps the symmetric mirror
  for every size. The ADM working buffers are also zero-initialised, as
  upstream Netflix/vmaf does since `1786bd961`, so a future out-of-region
  read yields a reproducible value. Frames of 33 pixels or more in both
  dimensions score exactly as before.
