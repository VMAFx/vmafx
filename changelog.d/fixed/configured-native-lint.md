- Local native lint now selects tracked sources from the configured Meson
  database, covering engine roots, C++ tools, tests and tracked vendors without
  inventing build commands for inactive backends. It preserves build metadata,
  adapts numeric GCC LTO flags only in a private analyzer copy, retains command
  variants and analyzer receipts, and runs cppcheck after clang-tidy failures.
