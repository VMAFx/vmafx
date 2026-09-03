/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Netflix/vmaf#743 regression test — the CLI progress spinner must not emit
 *  UTF-8 braille (or a bare CSI erase-to-EOL) to a console that cannot render
 *  it.
 *
 *  The CLI writes the spinner to stderr with a plain byte-oriented fprintf and
 *  never touched the Windows console output code page, so under the default
 *  OEM/ANSI pages the two braille glyphs decoded as mojibake — six garbage
 *  glyphs under cp437, an illegal multibyte sequence under cp936 — on every
 *  frame of every run.  The same fprintf also emitted `\033[K`
 *  unconditionally, which legacy conhost prints literally because
 *  ENABLE_VIRTUAL_TERMINAL_PROCESSING is off by default.
 *
 *  The CLI now switches the console to UTF-8 + VT for the duration of the run
 *  and restores the previous state on exit; when the console refuses either,
 *  the selectors below pick the ASCII table and space padding.  Driving the
 *  selectors with the code pages a real conhost reports is what makes this
 *  testable without a Windows box — the two Windows CI legs then prove the
 *  console-init code compiles and links.
 *
 *  The POSIX half matters just as much: the CLI passes
 *  SPINNER_CODEPAGE_UTF8 / vt_enabled=1 unconditionally there, so the emitted
 *  bytes must stay identical to the pre-fix build.
 */

#include <cstring>

#include "test.h"

#include "spinner.h"

namespace
{

/* Code pages a real Windows console reports by default. */
constexpr unsigned kCp437 = 437u;   /* US OEM — the conhost default */
constexpr unsigned kCp1252 = 1252u; /* Western ANSI */
constexpr unsigned kCp936 = 936u;   /* Simplified Chinese GBK */

bool table_is_ascii_only(const char *const *table, unsigned length)
{
    for (unsigned i = 0; i < length; i++) {
        for (const char *p = table[i]; *p; p++) {
            if (static_cast<unsigned char>(*p) > 0x7Fu)
                return false;
        }
    }
    return true;
}

const char *test_utf8_console_gets_the_braille_table()
{
    unsigned length = 0;
    const char *const *table = spinner_table_for_codepage(SPINNER_CODEPAGE_UTF8, &length);
    mu_assert("UTF-8 console must get the braille table", table == spinner);
    mu_assert("UTF-8 console must get the braille table length", length == spinner_length);
    mu_assert("the braille table must actually be non-ASCII", !table_is_ascii_only(table, length));
    return nullptr;
}

const char *test_legacy_code_pages_get_the_ascii_table()
{
    static const unsigned kCases[] = {kCp437, kCp1252, kCp936, 0u};
    for (const unsigned code_page : kCases) {
        unsigned length = 0;
        const char *const *table = spinner_table_for_codepage(code_page, &length);
        mu_assert("a non-UTF-8 console must get the ASCII table", table == spinner_ascii);
        mu_assert("a non-UTF-8 console must get the ASCII table length",
                  length == spinner_ascii_length);
        mu_assert("the fallback table must be pure ASCII", table_is_ascii_only(table, length));
    }
    return nullptr;
}

const char *test_erase_eol_is_vt_gated()
{
    mu_assert("a VT-capable console gets the CSI erase-to-EOL",
              std::strcmp(spinner_erase_eol(1), "\033[K") == 0);
    const char *fallback = spinner_erase_eol(0);
    mu_assert("a VT-less console must not get a CSI sequence",
              std::strchr(fallback, '\033') == nullptr);
    mu_assert("the VT-less fallback must still clear trailing characters",
              std::strlen(fallback) > 0);
    return nullptr;
}

/* Byte-for-byte anchor for the POSIX path: the first and last braille entries
 * must be exactly the bytes the pre-fix table carried, and every entry must be
 * two 3-byte U+28xx code points. */
const char *test_braille_table_bytes_unchanged()
{
    mu_assert("braille table must still have 56 entries", spinner_length == 56u);
    mu_assert("first entry must be U+2880 U+2800",
              std::strcmp(spinner[0], "\xe2\xa2\x80\xe2\xa0\x80") == 0);
    mu_assert("last entry must be U+2800 U+2840",
              std::strcmp(spinner[spinner_length - 1], "\xe2\xa0\x80\xe2\xa1\x80") == 0);
    for (const char *const entry : spinner) {
        mu_assert("every braille entry is two 3-byte code points", std::strlen(entry) == 6u);
    }
    return nullptr;
}

} // namespace

mu_message_t run_tests()
{
    mu_run_test(test_utf8_console_gets_the_braille_table);
    mu_run_test(test_legacy_code_pages_get_the_ascii_table);
    mu_run_test(test_erase_eol_is_vt_gated);
    mu_run_test(test_braille_table_bytes_unchanged);
    return nullptr;
}
