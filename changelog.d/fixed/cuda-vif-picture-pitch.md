- **The CUDA VIF twin reads each picture with its own row pitch.** `vif_cuda`
  read both input pictures with the pitch of the engine's own device
  pictures, so a CUDA picture with another pitch (a frame imported where its
  producer holds it) scored wrong VIF values; it now uses each picture's
  stride.
