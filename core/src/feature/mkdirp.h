
//
// mkdirp.h
//
// Copyright (c) 2013 Stephen Mathieson
// MIT licensed
//

#ifndef MKDIRP
#define MKDIRP

#include <sys/types.h>
#include <sys/stat.h>

/* MSVC's <sys/types.h> doesn't define POSIX mode_t. Provide a local
 * typedef so the public signature matches everywhere; on Windows the
 * value is ignored (CreateDirectoryA takes no mode). */
#if defined(_WIN32) && !defined(__MINGW32__)
typedef unsigned int mode_t;
#endif

/*
 * Recursively `mkdir(path, mode)`
 */

int mkdirp(const char *, mode_t);

#endif
