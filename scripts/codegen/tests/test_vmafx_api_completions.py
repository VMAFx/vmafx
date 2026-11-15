#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Shell completions and the man page (issue #2324).

Every spelling the command line offers completes in every shell and is named
in the man page; a value domain completes as the option's argument; an option
removed from the definition disappears from all five outputs; the per-shell
escapes survive the documentations' own quotes and roff control characters.
"""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from support import fixture, parse
from vmafx_api import emit_completions as completions
from vmafx_api.emit_cli import cli_options
from vmafx_api.model import CliSpelling, Option

from scripts.lib.safe_subprocess import run as run_command

BASH = "core/tools/completions/vmaf.bash"
ZSH = "core/tools/completions/vmaf.zsh"
FISH = "core/tools/completions/vmaf.fish"
PS1 = "core/tools/completions/vmaf.ps1"
MAN = "core/tools/man/vmaf.1"


def needle(path: str, flag: str) -> str:
    """How `flag` appears in the output of `path`: fish spells a long option as
    `-l name`, the man page roff-escapes the dashes, the rest carry it verbatim."""
    if path == FISH:
        return flag[2:]
    if path == MAN:
        return str(completions._roff(flag))
    return flag


def string_option(doc: str, **cli: object) -> Option:
    """A minimal cli-surface option for the escape tests."""
    return Option(
        name="x",
        type="string",
        default=None,
        doc=doc,
        since=(0, 1),
        deprecated=None,
        range=None,
        values=(),
        spellings={},
        proto_field=0,
        surfaces=("cli",),
        choices=(),
        repeat=(),
        default_macro="",
        cli=CliSpelling(flags=("--x",), short="x", meta="$string"),
    )


class SpellingCoverageTests(unittest.TestCase):
    """The boundary acceptance: aliases and underscore spellings complete."""

    def setUp(self) -> None:
        self.api = parse(fixture())
        self.outputs = {
            path: renderer(self.api) for path, renderer in completions.OUTPUT_TEXTS.items()
        }
        self.switches = [
            flag
            for option in cli_options(self.api)
            if option.cli is not None and option.cli.values
            for _, flag in option.cli.values
        ]

    def test_every_spelling_appears_in_every_output(self) -> None:
        for option in cli_options(self.api):
            assert option.cli is not None
            for flag in [*option.cli.flags, *self.switches]:
                # fish spells a long option as `-l name`, without the dashes; the
                # other surfaces carry the flag verbatim.
                for path, text in self.outputs.items():
                    self.assertIn(needle(path, flag), text, f"{flag} is missing from {path}")

    def test_aliases_complete_alongside_the_primary(self) -> None:
        # The fixture's `precise` option spells "--precise" and the alias
        # "--precise-scores"; both must complete, in every shell (fish without
        # the dashes).
        for path, text in self.outputs.items():
            for flag in ("--precise", "--precise-scores"):
                self.assertIn(needle(path, flag), text, f"{flag} is missing from {path}")

    def test_both_command_names_are_registered(self) -> None:
        self.assertIn("complete -o filenames -F _vmaf vmaf", self.outputs[BASH])
        self.assertIn("complete -o filenames -F _vmaf vmafx", self.outputs[BASH])
        self.assertIn("#compdef vmaf vmafx", self.outputs[ZSH])
        self.assertIn("'vmaf','vmafx'", self.outputs[PS1])
        for line in self.outputs[FISH].splitlines():
            if line.startswith("complete "):
                self.assertIn("-c vmaf -c vmafx", line)


class ValueDomainTests(unittest.TestCase):
    """Completing `--back<TAB>` yields `--backend`; its argument yields the domain."""

    def setUp(self) -> None:
        self.api = parse(fixture())
        self.outputs = {
            path: renderer(self.api) for path, renderer in completions.OUTPUT_TEXTS.items()
        }

    def test_prefix_match_yields_the_option_in_the_flag_lists(self) -> None:
        # The compgen word lists and the flags arrays carry the whole spelling, so
        # the shell's own prefix match answers `--bitd<TAB>` with `--bitdepth`.
        for path in (BASH, PS1):
            self.assertIn("--bitdepth", self.outputs[path], path)

    def test_int_choices_complete_as_the_argument(self) -> None:
        # The fixture's `bitdepth` option is an integer with choices [8, 10] and
        # the short `-b`; both spellings arm the argument completion.
        self.assertIn('  ["--bitdepth"]="8 10"', self.outputs[BASH])
        self.assertIn('  ["-b"]="8 10"', self.outputs[BASH])
        self.assertIn(":$unsigned:(8 10)", self.outputs[ZSH])
        self.assertIn("-a '8 10'", self.outputs[FISH])
        self.assertIn("'--bitdepth' = @('8','10')", self.outputs[PS1])
        self.assertIn("'-b' = @('8','10')", self.outputs[PS1])
        self.assertIn("One of: 8|\\:10.", self.outputs[MAN])

    def test_enum_values_complete_as_the_argument(self) -> None:
        # The fixture's `pool` option is an enum; its values complete.
        self.assertIn('  ["--pool"]="mean min harmonic_mean"', self.outputs[BASH])
        self.assertIn(":$string:(mean min harmonic_mean)", self.outputs[ZSH])
        self.assertIn("-a 'mean min harmonic_mean'", self.outputs[FISH])
        self.assertIn("'--pool' = @('mean','min','harmonic_mean')", self.outputs[PS1])
        self.assertIn("One of: mean|\\:min|\\:harmonic_mean.", self.outputs[MAN])

    def test_value_switches_complete_as_switches(self) -> None:
        # The fixture's `format` option spells its values as switches (--xml/--json),
        # so no value entry or argument completion exists for a --format argument.
        for path in (BASH, FISH, PS1, MAN):
            self.assertIn(needle(path, "--json"), self.outputs[path], path)
        self.assertIn('  ["--bitdepth"]="8 10"', self.outputs[BASH])
        self.assertNotIn('  ["--format"]="', self.outputs[BASH])

    def test_every_value_word_goes_through_the_compgen_helper(self) -> None:
        # shellcheck's SC2207 fires on `COMPREPLY=( $(compgen ...) )`, so the
        # generated script fills the array through one helper instead; the
        # substitution form must never reappear.
        self.assertIn('done < <(compgen "$@")', self.outputs[BASH])
        self.assertNotIn("COMPREPLY=( $(", self.outputs[BASH])

    def test_bash_and_powershell_key_the_domain_by_every_spelling(self) -> None:
        # Aliases and the short spell the same option, so `-p <TAB>` and
        # `--the_alias <TAB>` must complete the same argument words.
        doc = fixture()
        group = next(group for group in doc["option_groups"] if group["name"] == "pool")
        group["options"][0]["cli"] = ["--pool", "--pooling"]
        group["options"][0]["cli_short"] = "P"
        api = parse(doc)
        bash = completions.bash_text(api)
        self.assertIn('  ["--pool"]="mean min harmonic_mean"', bash)
        self.assertIn('  ["--pooling"]="mean min harmonic_mean"', bash)
        self.assertIn('  ["-P"]="mean min harmonic_mean"', bash)
        ps1 = completions.powershell_text(api)
        self.assertIn("'--pool' = @('mean','min','harmonic_mean')", ps1)
        self.assertIn("'--pooling' = @('mean','min','harmonic_mean')", ps1)
        self.assertIn("'-P' = @('mean','min','harmonic_mean')", ps1)

    def test_path_meta_completes_files_and_numbers_do_not(self) -> None:
        self.assertIn(":$path:_files", self.outputs[ZSH])
        self.assertNotIn(":$unsigned:_files", self.outputs[ZSH])
        # bash: a path option has no domain entry, so the non-dash branch
        # completes files.
        self.assertNotIn('  ["--reference"]="', self.outputs[BASH])

    def test_the_value_table_is_declared_global(self) -> None:
        # bash-completion sources the file inside a function, so a plain
        # `declare -A` would be local and gone before `_vmaf` runs; the
        # non-empty guard keeps an empty previous word from expanding an
        # empty subscript (`bad array subscript` on the user's prompt).
        self.assertIn("declare -gA _vmaf_values=(", self.outputs[BASH])
        self.assertIn(
            'if [[ -n "${prev}" && -n "${_vmaf_values[${prev}]+set}" ]]; then',
            self.outputs[BASH],
        )

    def test_man_names_the_short_after_the_long_flags(self) -> None:
        # The reference option carries the short `-r`; the names line reads
        # `--reference, -r` with no stray separator between the two spellings.
        self.assertNotIn(", / ", self.outputs[MAN])
        self.assertIn("\\-\\-reference, \\-r", self.outputs[MAN])


class RemovedOptionDriftTests(unittest.TestCase):
    """The negative acceptance: an option removed from the definition disappears
    from all five outputs, and the committed-file drift check fails on a stale
    output."""

    def removed_pool(self) -> object:
        doc = fixture()
        pool = next(group for group in doc["option_groups"] if group["name"] == "pool")
        pool["options"] = [option for option in pool["options"] if option["name"] != "pool"]
        return parse(doc)

    def test_removed_option_disappears_from_every_output(self) -> None:
        full = {path: renderer(self.api()) for path, renderer in completions.OUTPUT_TEXTS.items()}
        trimmed = {
            path: renderer(self.removed_pool())
            for path, renderer in completions.OUTPUT_TEXTS.items()
        }
        for path in full:
            self.assertIn(needle(path, "--pool"), full[path], path)
            self.assertNotIn(needle(path, "--pool"), trimmed[path], path)
            self.assertIn(needle(path, "--subsample"), trimmed[path], path)

    def api(self) -> object:
        return parse(fixture())


class HookCleanlinessTests(unittest.TestCase):
    """The generated files must already be in the style the repository's hooks
    enforce (shfmt -i 2 -ci rewrites nothing, zsh parses), since a generated
    file cannot be hand-formatted after the fact."""

    def setUp(self) -> None:
        api = parse(fixture())
        self.outputs = {path: renderer(api) for path, renderer in completions.OUTPUT_TEXTS.items()}

    @unittest.skipUnless(shutil.which("shfmt"), "shfmt is not installed")
    def test_bash_is_shfmt_clean(self) -> None:
        # The repository formats every tracked shell file with shfmt -i 2 -ci
        # (hook shfmt-src); the generated script must already be in that style.
        # Written to a file because the hook formats files, and stdin `-d` is
        # unreliable on Windows.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vmaf.bash"
            path.write_text(self.outputs[BASH], encoding="utf-8", newline="\n")
            result = run_command(
                ["shfmt", "-i", "2", "-ci", "-d", str(path)],
                allowed_executables=["shfmt"],
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 0, result.stdout)

    @unittest.skipUnless(shutil.which("zsh"), "zsh is not installed")
    def test_zsh_parses(self) -> None:
        # ShellCheck has no zsh mode and the pre-commit hook excludes the file,
        # so `zsh -n` is the generated script's only syntax guard.
        result = run_command(
            ["zsh", "-n"],
            allowed_executables=["zsh"],
            input_data=self.outputs[ZSH],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which("bash"), "bash is not installed")
    def test_empty_previous_word_prints_nothing(self) -> None:
        # `vmaf "" <Tab>` makes the domain lookup expand an empty subscript and
        # bash prints `bad array subscript` to the user's prompt in the middle
        # of the completion. The guard keeps the subscript unevaluated, so the
        # completion falls through to file names silently.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vmaf.bash"
            path.write_text(self.outputs[BASH], encoding="utf-8", newline="\n")
            script = (
                f"source '{path.as_posix()}'; "
                # On bash < 4.2 the file returns before defining `_vmaf`; sourcing
                # it must still print nothing.
                "declare -F _vmaf >/dev/null || exit 0; "
                "COMP_WORDS=(vmaf '' ''); COMP_CWORD=2; _vmaf >/dev/null"
            )
            result = run_command(
                ["bash", "-c", script],
                allowed_executables=["bash"],
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    @unittest.skipUnless(shutil.which("bash"), "bash is not installed")
    def test_equals_form_completes_the_domain(self) -> None:
        # `vmaf --bitdepth=8<Tab>` puts the option and its argument in one word;
        # the boundary is normalized so the lookup sees `--backend`, without
        # touching the user's COMP_WORDBREAKS. On bash < 4.2 the file returns
        # before defining `_vmaf`, and this test only pins a silent source.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vmaf.bash"
            path.write_text(self.outputs[BASH], encoding="utf-8", newline="\n")
            script = (
                f"source '{path.as_posix()}'; "
                "declare -F _vmaf >/dev/null || exit 0; "
                "echo DEFINED; "
                "COMP_WORDS=(vmaf '--bitdepth='); COMP_CWORD=1; _vmaf; "
                'printf "%s\\n" "${COMPREPLY[@]}"'
            )
            result = run_command(
                ["bash", "-c", script],
                allowed_executables=["bash"],
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        if "DEFINED" in result.stdout:
            self.assertIn("8", result.stdout)
            self.assertIn("10", result.stdout)

    def test_files_carry_the_license_header(self) -> None:
        # Every new file starts with a license header (ADR-3401: the outside
        # contributor holds the copyright); the zsh file keeps `#compdef` as
        # its first line or the completion function never autoloads.
        for path, text in self.outputs.items():
            self.assertIn("Copyright 2026 luoye-cpu", text, path)
            self.assertIn("SPDX-License-Identifier: EUPL-1.2", text, path)
        self.assertTrue(self.outputs[ZSH].startswith("#compdef vmaf vmafx\n"))

    def test_bash_returns_early_on_older_shells(self) -> None:
        # macOS ships bash 3.2 as /bin/bash; it has neither `declare -A` nor
        # `declare -g`, so the file returns before either appears and older
        # shells get no completion instead of errors at startup.
        self.assertIn("if ((BASH_VERSINFO[0] < 4", self.outputs[BASH])
        self.assertIn("  return 0", self.outputs[BASH])

    def test_zsh_declares_the_equals_form_for_value_options(self) -> None:
        # The plain spec covers `--opt value`; zsh only completes `--opt=value`
        # when a spec declares the equals form.
        self.assertIn("'--bitdepth=[Bits per sample.]:$unsigned:(8 10)'", self.outputs[ZSH])

    def test_powershell_selects_the_previous_word_from_the_cursor(self) -> None:
        # `Count - 2` picks the word before the option (PowerShell gives no
        # empty trailing element); element extents and the cursor position
        # name the option instead.
        self.assertIn("Extent.EndOffset -le $cursorPosition", self.outputs[PS1])
        self.assertNotIn("Count - 2", self.outputs[PS1])


class EscapeTests(unittest.TestCase):
    """The per-shell escapes survive the documentation's own quotes and roff
    control characters (the pixel-format docs carry `Y'CbCr` upstream; here a
    synthetic doc pins each shell's rule)."""

    def test_zsh_escapes_quote_and_bracket(self) -> None:
        spelling = completions._Spelling(string_option("The model's [x] range; use $var."))
        spec = completions._zsh_spec(spelling, "--x")
        # The apostrophe becomes the standard '\'' close-escape-open sequence, the
        # description block's ] is backslash-escaped and the $ stays literal.
        self.assertIn("The model'\\''s [x\\] range; use \\$var.", spec)

    def test_fish_escapes_quote_and_backslash(self) -> None:
        self.assertEqual(completions._fish("a 'b' c\\d"), "a \\'b\\' c\\\\d")

    def test_powershell_doubles_quotes(self) -> None:
        self.assertEqual(completions._ps("a 'b'"), "'a ''b'''")

    def test_roff_escapes_dashes_control_characters_and_leading_dot(self) -> None:
        self.assertEqual(completions._roff("--backend"), "\\-\\-backend")
        self.assertEqual(completions._roff("a\\b"), "a\\eb")
        self.assertEqual(completions._roff(".TP not here"), "\\&.TP not here")

    def test_man_meta_font_escapes_survive_the_line_escaper(self) -> None:
        api = parse(fixture())
        text = completions.man_text(api)
        self.assertIn("\\fI$path\\fR", text)
        self.assertNotIn("\\efI", text)


if __name__ == "__main__":
    unittest.main()
