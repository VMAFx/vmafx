- **Cppcheck pthread modeling:** Load the official POSIX API model in local
  and CI analysis so shared C aggregates are not mistaken for C++ classes
  needing constructors. Real-header controls still reject uninitialized
  member reads and broken constructors; target settings and diagnostic
  categories remain unchanged.
