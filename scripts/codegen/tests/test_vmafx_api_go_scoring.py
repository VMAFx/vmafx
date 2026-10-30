# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Go binding wraps the model, frame and scoring calls pkg/libvmaf runs on (#2434)."""

from __future__ import annotations

import unittest

from support import DEFINITION, document
from vmafx_api import emit_go
from vmafx_api.loader import parse


def live_go() -> str:
    return emit_go.api_text(parse(document(DEFINITION)))


class GoScoringBindingTest(unittest.TestCase):
    def test_model_frame_and_scoring_wrappers_are_emitted(self) -> None:
        text = live_go()
        for needle in (
            "type Model struct {",
            "func LoadModelFile(config *ModelConfig, path string) (*Model, error) {",
            "func (m *Model) Close() {",
            "type Frame struct {",
            "func NewHostFrame(desc FrameDesc) (*Frame, error) {",
            "func (f *Frame) PlaneBytes(plane uint32) ([]byte, error) {",
            "func (c *Context) UseModel(model *Model) error {",
            "func (c *Context) Submit(reference *Frame, distorted *Frame, index uint64) error {",
            "func (c *Context) ScorePooled(model *Model, pool uint32, first uint64, last uint64)",
            "func (c *Context) ProvenanceJson(flags uint32) (string, error) {",
        ):
            self.assertIn(needle, text)

    def test_only_submit_consumes_its_frames(self) -> None:
        text = live_go()
        submit = text[text.index("func (c *Context) Submit(") :].split("\n}\n", 1)[0]
        self.assertIn("reference.takeHandle()", submit)
        self.assertIn("distorted.takeHandle()", submit)
        rest = text.replace(submit, "")
        self.assertNotIn(".takeHandle()", rest.replace("func (f *Frame) takeHandle()", ""))

    def test_locale_is_pinned_once_before_any_call(self) -> None:
        text = live_go()
        self.assertEqual(text.count("func init() {"), 1)
        self.assertIn('setlocale(LC_NUMERIC, "C")', text)

    def test_output_is_deterministic(self) -> None:
        self.assertEqual(live_go(), live_go())

    def test_calls_with_unbound_parameter_shapes_are_not_wrapped(self) -> None:
        """A model set, a callback or an options list has no Go shape yet: the
        call stays out of the binding rather than being bound wrongly."""
        text = live_go()
        for name in ("UseModelSet", "UseFeature(", "ImportFrame", "WindowSubmit", "UseDevice"):
            self.assertNotIn(f"func (c *Context) {name}", text)


if __name__ == "__main__":
    unittest.main()
