### HIP PSNR memory copy direction

Fixed `integer_psnr_hip.c` to use `hipMemcpyHostToDevice` (not `hipMemcpyDeviceToDevice`) for host-to-device copies of reference and distortion picture planes during submit. The incorrect enum silently corrupted results or triggered driver faults on affected HIP runtimes.
