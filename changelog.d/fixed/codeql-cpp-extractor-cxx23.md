- CI: the CodeQL C/C++ analysis builds the tree as C++23. The extractor read
  `-std=c++26` as C++17 and reported 16 alerts in `core/tools/cli_parse.cpp`
  on globals and unused statics that do not exist
  ([ADR-3355](docs/adr/3355-codeql-cpp-extractor-cxx23.md)).
