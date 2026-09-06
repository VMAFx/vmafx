/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#ifndef VMAF_SPINNER_H
#define VMAF_SPINNER_H

/* ADR-0809: static gives internal linkage in both C and C++ so the header
 * can be included in a C++ TU without an ODR violation if ever included in
 * more than one translation unit. */

/*
 * Progress-spinner glyph tables for the interactive (`isatty`) CLI display.
 *
 * The braille table is UTF-8. Netflix/vmaf#743 reports that it renders as
 * mojibake on Windows, because the CLI writes raw bytes to stderr with
 * fprintf and never touches the console output code page: under the default
 * OEM/ANSI pages the two-glyph spinner decodes as
 *
 *   cp437  -> six garbage glyphs, so the progress line is four characters
 *              wider than the `\r` overwrite assumes
 *   cp1252 -> six garbage glyphs (a different set)
 *   cp936  -> an illegal multibyte sequence; conhost draws replacement boxes
 *
 * and it repeats on every frame for the whole run. The CLI now switches the
 * console to UTF-8 for the duration of the run (restoring the previous code
 * page on exit) and falls back to the ASCII table below when the console
 * refuses UTF-8 or VT sequences. On POSIX the selector always returns the
 * braille table, so output is byte-identical to before.
 */
static const char *const spinner[] = {
    "⢀⠀", "⡀⠀", "⠄⠀", "⢂⠀", "⡂⠀", "⠅⠀", "⢃⠀", "⡃⠀", "⠍⠀", "⢋⠀", "⡋⠀", "⠍⠁", "⢋⠁", "⡋⠁",
    "⠍⠉", "⠋⠉", "⠋⠉", "⠉⠙", "⠉⠙", "⠉⠩", "⠈⢙", "⠈⡙", "⢈⠩", "⡀⢙", "⠄⡙", "⢂⠩", "⡂⢘", "⠅⡘",
    "⢃⠨", "⡃⢐", "⠍⡐", "⢋⠠", "⡋⢀", "⠍⡁", "⢋⠁", "⡋⠁", "⠍⠉", "⠋⠉", "⠋⠉", "⠉⠙", "⠉⠙", "⠉⠩",
    "⠈⢙", "⠈⡙", "⠈⠩", "⠀⢙", "⠀⡙", "⠀⠩", "⠀⢘", "⠀⡘", "⠀⠨", "⠀⢐", "⠀⡐", "⠀⠠", "⠀⢀", "⠀⡀"};

static const unsigned spinner_length = sizeof(spinner) / sizeof(spinner[0]);

/* Pure-ASCII fallback: safe under every Windows console code page. */
static const char *const spinner_ascii[] = {"|", "/", "-", "\\"};

static const unsigned spinner_ascii_length = sizeof(spinner_ascii) / sizeof(spinner_ascii[0]);

/* Windows code page for UTF-8 (CP_UTF8). Spelled out so this header stays
 * usable without <windows.h>, and so the POSIX build can pass it verbatim. */
#define SPINNER_CODEPAGE_UTF8 65001u

/*
 * Pick the glyph table for a console output code page. Anything other than
 * UTF-8 gets the ASCII table; POSIX callers pass SPINNER_CODEPAGE_UTF8
 * unconditionally, which keeps their output byte-identical to before.
 */
static inline const char *const *spinner_table_for_codepage(unsigned code_page, unsigned *length)
{
    if (code_page == SPINNER_CODEPAGE_UTF8) {
        *length = spinner_length;
        return spinner;
    }
    *length = spinner_ascii_length;
    return spinner_ascii;
}

/*
 * Erase-to-end-of-line for the progress line. Legacy conhost has
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING off by default and prints the CSI
 * sequence literally as "<-[K"; pad with spaces there instead.
 */
static inline const char *spinner_erase_eol(int vt_enabled)
{
    return vt_enabled ? "\033[K" : "        ";
}

#endif // VMAF_SPINNER_H
