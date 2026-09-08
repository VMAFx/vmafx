# PSNR format-table analyzer cleanup

The configured Cppcheck run flagged the private C++ `FormatEntry::params`
member as lacking a default initializer and suggested a standard lookup
algorithm. All twelve entries in the existing `constexpr` table already
initialize both fields; no uninitialized runtime access was demonstrated.

Give the private member an empty default initializer and use
`std::ranges::find` projected through `FormatEntry::fmt`. The first exact
string match still supplies the same constants. The optional lookup result
is read-only. Removing an unnecessary uncited `cert-err58-cpp` marker leaves
the complete translation unit clean without a replacement suppression.

The configured CPU C++26 compiler builds both original and changed source.
A differential probe verifies 68 cases: twelve supported formats, four
unsupported strings and a null format, each with ordinary output pointers,
a null peak, a null ceiling and aliased outputs. Return codes and output
double bytes match. All twelve table initializer rows are textually unchanged.
Actual clang-tidy and Cppcheck report no source diagnostics; the scoped
baseline writer removes one uncited marker allowance and changes no other
source allowance.

No new policy, alternatives or rebase-sensitive invariant is introduced.
The existing C entry point and header remain unchanged; no FFmpeg patch or
human usage change is needed. The probe, native compiler commands and raw
analyzer receipts are retained under
`.workingdir2/evidence/psnr-format-table-lint-20260908/`.
