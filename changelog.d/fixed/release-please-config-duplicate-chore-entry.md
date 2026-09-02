Remove duplicate `chore` entry in `ai` package `changelog-sections` array that caused a
JSON parse error at line 64 column 9, breaking every release-please workflow run.

Split draft-release creation from next-PR generation so release-please does not
try to resolve a tag that intentionally remains absent until an operator publishes
the draft.
