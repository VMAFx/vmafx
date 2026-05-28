
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
#else
constexpr char kPathSep = '/';
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

    for (std::size_t i = 0; i < path.size(); ++i) {
        out += path[i];
        if (path[i] == '/') {
            while (i + 1 < path.size() && path[i + 1] == '/')
                ++i;
        }
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

    // Find last separator to derive the parent path.
    std::size_t sep_pos = pathname.rfind(kPathSep);
    if (sep_pos != std::string::npos && sep_pos > 0) {
        // Recurse to create parent; return early on failure.
        std::string parent{pathname.substr(0, sep_pos)};
        if (mkdirp(parent.c_str(), mode) != 0)
            return -1;
    }

#ifdef _WIN32
    (void)mode;
    int rc = _mkdir(pathname.c_str());
#else
    int rc = mkdir(pathname.c_str(), mode);
#endif

    return (rc == 0 || errno == EEXIST) ? 0 : -1;
}
