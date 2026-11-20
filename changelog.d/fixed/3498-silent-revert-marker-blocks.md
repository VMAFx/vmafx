- **The Silent-Revert Guard now treats generator marker blocks as generated output.** Lines strictly between
  `<!-- name:begin -->` or `<!-- name:table ... -->` and `<!-- name:end -->` are no longer reported when a merge
  regenerates them; the generator input remains protected, hand-written prose outside the markers is still checked,
  and unterminated or nested marker blocks fail closed instead of silently passing.
