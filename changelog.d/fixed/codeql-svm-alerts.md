Eliminate CodeQL alerts 1222–1225 (cpp/resource-not-released-in-destructor)
and 1226 (cpp/loop-variable-changed) in vendored libsvm (core/src/svm.cpp).
Solver heap buffers are managed via idempotent solve_cleanup() called by
solve_finish(), ~Solver(), and solve_setup() with deleted copy operations,
preventing exception leaks and double-free hazards. The support-vector parser
loop is converted to a bounded while loop with sentinel validation (ADR-0889,
Research-2094).
