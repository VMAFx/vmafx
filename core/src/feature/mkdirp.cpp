
//
// mkdirp.cpp
//
// Original C implementation: Copyright (c) 2013 Stephen Mathieson, MIT licensed.
// C++23 conversion: Copyright 2026 Lusoris (ADR-0731 / cpp23 Wave 3 part B).
//
// Migration note (ADR-0731):
//   - `git mv mkdirp.c mkdirp.cpp` preserves blame.
//   - The public C API `mkdirp()` is unchanged; mkdirp.h declares it under
//     `extern "C"` so every C caller links without modification.
//   - `path_normalize` is rewritten using `std::string`, eliminating the
//     manual `strdup` + pointer arithmetic and the two separate `free()` calls
//     that the C version required for both success and goto-fail paths.
//   - RAII (`std::string`) replaces the `goto fail` cleanup pattern, making
//     every exit path — including early returns — leak-free by construction.
//   - Recursion replaced with an iterative prefix-walk (Power of 10 #1;
//     adversarial review 2026-05-28 finding #11).
//   - `nullptr` replaces `NULL` in C++ code.
//   - `std::string_view` is used for the normalized path to avoid copies when
//     finding the last separator.
//

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mkdirp.h"

namespace
{

#ifdef _WIN32
constexpr char kPathSep = '\\';

[[nodiscard]] constexpr bool is_path_separator(char ch) noexcept
{
    return ch == '/' || ch == '\\';
}

[[nodiscard]] constexpr bool starts_unc_prefix(std::string_view path, std::size_t pos) noexcept
{
    return pos == 0 && path.size() > 1 && is_path_separator(path[0]) && is_path_separator(path[1]);
}

static_assert(starts_unc_prefix("\\\\server\\share", 0));
#else
constexpr char kPathSep = '/';

[[nodiscard]] constexpr bool is_path_separator(char ch) noexcept
{
    return ch == '/';
}
#endif

/*
 * Collapse consecutive path separators to a single separator.
 * Returns the normalized path, or an empty string on allocation failure.
 */
[[nodiscard]] static std::string path_normalize(std::string_view path)
{
    if (path.empty())
        return {};

    std::string out;
    out.reserve(path.size());

    std::size_t i = 0;
    while (i < path.size()) {
        const bool separator = is_path_separator(path[i]);
        out += separator ? kPathSep : path[i];
        if (separator) {
            /* A leading separator pair is structural on Windows: it starts a
             * UNC path (\\\\server\\share) or an extended-length path
             * (\\\\?\\...). Preserve the pair while still collapsing any
             * additional leading separators and all later redundant runs. */
#ifdef _WIN32
            if (starts_unc_prefix(path, i)) {
                ++i;
                continue;
            }
#endif
            while (i + 1 < path.size() && is_path_separator(path[i + 1]))
                ++i;
        }
        ++i;
    }
    return out;
}

} // namespace

extern "C" int mkdirp(const char *path, mode_t mode)
{
    if (!path)
        return -1;

    std::string pathname = path_normalize(path);
    if (pathname.empty())
        return -1;

    /* Iterative path-component creation (Power of 10 #1 — no recursion).
     * Walk the normalised path left-to-right, creating each prefix component
     * before attempting the final directory.  Bounded by the number of '/'
     * separators, which is at most strlen(path) — statically verifiable
     * upper bound (adversarial review 2026-05-28 finding #11). */
    for (std::size_t pos = 1; pos <= pathname.size(); ++pos) {
        if (pos < pathname.size() && pathname[pos] != kPathSep)
            continue;
        /* Create the prefix [0, pos). */
        const std::string prefix = pathname.substr(0, pos);
#ifdef _WIN32
        (void)mode;
        const int rc = _mkdir(prefix.c_str());
#else
        const int rc = mkdir(prefix.c_str(), mode);
#endif
        if (rc != 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}
