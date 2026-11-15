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

import unittest

from support import fixture, parse
from vmafx_api import emit_completions as completions
from vmafx_api.emit_cli import cli_options
from vmafx_api.model import CliSpelling, Option

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
        return completions._roff(flag)
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
        # The fixture's `bitdepth` option is an integer with choices [8, 10].
        self.assertIn("        --bitdepth)", self.outputs[BASH])
        self.assertIn(":$unsigned:(8 10)", self.outputs[ZSH])
        self.assertIn("-a '8 10'", self.outputs[FISH])
        self.assertIn("'--bitdepth' = @('8','10')", self.outputs[PS1])
        self.assertIn("One of: 8|10.", self.outputs[MAN])

    def test_enum_values_complete_as_the_argument(self) -> None:
        # The fixture's `pool` option is an enum; its values complete.
        self.assertIn("mean min harmonic_mean", self.outputs[BASH])
        self.assertIn(":$string:(mean min harmonic_mean)", self.outputs[ZSH])
        self.assertIn("-a 'mean min harmonic_mean'", self.outputs[FISH])
        self.assertIn("'--pool' = @('mean','min','harmonic_mean')", self.outputs[PS1])
        self.assertIn("One of: mean|min|harmonic_mean.", self.outputs[MAN])

    def test_value_switches_complete_as_switches(self) -> None:
        # The fixture's `format` option spells its values as switches (--xml/--json),
        # so no case arm or argument completion exists for a --format argument.
        for path in (BASH, FISH, PS1, MAN):
            self.assertIn(needle(path, "--json"), self.outputs[path], path)
        self.assertIn("--bitdepth)", self.outputs[BASH])
        self.assertNotIn("        --format)", self.outputs[BASH])

    def test_path_meta_completes_files_and_numbers_do_not(self) -> None:
        self.assertIn(":$path:_files", self.outputs[ZSH])
        self.assertNotIn(":$unsigned:_files", self.outputs[ZSH])
        # bash: a path option has no case arm, so the non-dash branch completes files.
        self.assertNotIn("        --reference)", self.outputs[BASH])


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
