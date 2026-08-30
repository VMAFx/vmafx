
//
// mkdirp.c
//
// Copyright (c) 2013 Stephen Mathieson
// MIT licensed
//

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
/* MSVC has no <unistd.h>; mkdir lives in <direct.h> as _mkdir(path).
 * The mode parameter is ignored on Windows (CreateDirectoryA path). */
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "mkdirp.h"

static char *path_normalize(const char *path)
{
    if (!path)
        return NULL;

    char *copy = strdup(path);
    if (NULL == copy)
        return NULL;
    char *ptr = copy;

    /* Loop variable 'i' is intentionally modified inside the body to skip
     * runs of consecutive path separators (e.g. "a//b" → "a/b"). The
     * ++i / --i pairs advance past duplicates then back up one so the
     * outer i++ lands on the character after the separator run.
     * CodeQL cpp/loop-variable-changed: accepted, intentional. */
    for (int i = 0; copy[i]; i++) {
        char c = path[i];
#ifdef _WIN32
        /* Treat POSIX '/' as the native '\\' so callers can mix separators
         * (e.g. WIN tmpdir + "/a/b/c" suffix from cross-platform tests). */
        if ('/' == c)
            c = '\\';
        *ptr++ = c;
        if ('\\' == c) {
            i++;
            while ('/' == path[i] || '\\' == path[i])
                i++;
            i--;
        }
#else
        *ptr++ = c;
        if ('/' == c) {
            i++;
            while ('/' == path[i])
                i++;
            i--;
        }
#endif
    }

    *ptr = '\0';

    return copy;
}

#ifdef _WIN32
#define PATH_SEPARATOR '\\'
#else
#define PATH_SEPARATOR '/'
#endif

int mkdirp(const char *path, mode_t mode)
{
    char *pathname = NULL;
    char *parent = NULL;

    if (NULL == path)
        return -1;

    pathname = path_normalize(path);
    if (NULL == pathname)
        goto fail;

    parent = strdup(pathname);
    if (NULL == parent)
        goto fail;

    char *p = parent + strlen(parent);
    while (PATH_SEPARATOR != *p && p != parent) {
        p--;
    }
    *p = '\0';

    // make parent dir
    if (p != parent && 0 != mkdirp(parent, mode))
        goto fail;
    free(parent);

// make this one if parent has been made
#ifdef _WIN32
    // http://msdn.microsoft.com/en-us/library/2fkk4dzw.aspx
    (void)mode;
    int rc = _mkdir(pathname);
#else
    int rc = mkdir(pathname, mode);
#endif

    free(pathname);

    return 0 == rc || EEXIST == errno ? 0 : -1;

fail:
    free(pathname);
    free(parent);
    return -1;
}
