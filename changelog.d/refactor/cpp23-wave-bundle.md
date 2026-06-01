- **C++23 Wave bundle** (#319 log.c→.cpp, #232 gpu_dispatch_env.c→.cpp,
  #136 opt.cpp+read_json_model.cpp, #154 feature_extractor.c→.cpp,
  #198 cli_parse.c+vmaf.c→.cpp): five sequential file-rename conversions
  bundled into a single PR. Each TU compiles in an isolated C++23 static
  library; public C ABI is preserved throughout via `extern "C"` guards.
  ADRs: 0708, 0858, 0761, 0772, 0809.
