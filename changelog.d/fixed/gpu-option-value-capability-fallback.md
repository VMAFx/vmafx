- Fall back to the CPU reference before GPU initialization when a model uses a
  valid non-default option value that the selected GPU twin does not yet
  implement. This covers `float_vif.vif_kernelscale`, every GPU
  `float_adm.adm_csf_mode`, and Metal `integer_adm.adm_csf_mode` while keeping
  CPU/GPU collector keys identical. Context creation also releases extractor
  private state when rejecting options on an optionless extractor.
