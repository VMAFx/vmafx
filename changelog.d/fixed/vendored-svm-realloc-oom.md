Fix three CERT MEM04-C realloc OOM defects in vendored libsvm (core/src/svm.cpp):
Cache::get_data, svm_group_classes, and svm_check_parameter all overwrote the source
pointer with the realloc return value; replaced with save-temp/check-NULL/abort idiom
consistent with existing Malloc behaviour in the file (ADR-1039).
