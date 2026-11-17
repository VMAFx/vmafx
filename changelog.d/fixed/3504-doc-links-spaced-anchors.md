- **The documentation link checker recognises spaced attribute-list anchors (`{ #id }`).**
  `scripts/docs/check_doc_links.py` collected only the unspaced `{#id}` attribute-list
  form, so headings written with whitespace inside the braces — valid attr_list syntax
  that MkDocs renders, and the form the split-page pointer headings of
  `docs/metrics/speed_qa.md` use — were invisible as anchors: the checker exited 1 on
  `docs/metrics` with two false missing-anchor findings for
  `speed_qa.md#the-scores-are-netflixs`, six over the whole docs tree (#3504). The
  anchor collector and the heading-slug strip now accept other attr_list attributes
  around the ID, so `{ #id }` and mixed lists such as `{ .class #id }` anchor exactly
  like `{#id}`, and the spaced form no longer leaks into the heading slug. A planted
  link to an ID that does not exist is still reported.
