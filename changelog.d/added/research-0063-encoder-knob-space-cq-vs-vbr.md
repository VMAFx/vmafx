- **Research-0063: encoder knob-space stratifies by rate-control mode.**
  The conventional VOD-HQ recipe (`-tune hq -multipass fullres
  -spatial_aq -temporal_aq -rc-lookahead 32 -bf 3` for NVENC;
  `-look_ahead -bf 4 -adaptive_i -adaptive_b` for QSV) is calibrated
  for VBR/CBR rate control. At constant CQ it **regresses** NVENC
  quality by 2.7–3.3 VMAF points (h264/hevc; av1 ~flat) and only
  marginally lifts QSV (+0.2 to +0.9). Implication: vmaf-tune's
  recommend output must carry a `rate_control_mode` field; the
  corpus tooling must tag each row with the mode it was generated
  under. fr_regressor_v2's input vector should gain a
  `rate_control_mode` one-hot. Documented as the prerequisite for
  Phase B's recommend command landing safely.
