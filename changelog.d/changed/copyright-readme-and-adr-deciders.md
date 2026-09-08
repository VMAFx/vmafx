- **Removed the remaining "Anthropic Claude" co-authorship claims.** ADR-0861
  dropped `and Claude (Anthropic)` from every per-file copyright notice but two
  surfaces were missed: the README's **Maintainers** line, which sat directly
  under the License bullet and so read as a rights claim, and the
  `- **Deciders**:` header line in 461 ADRs. Both now name the maintainer only.
  References to Claude as a *product* (for example ADR-0618's evaluation of the
  Claude Vision API) are technical citations, not attribution, and are
  untouched.
