- Enforce the documented 512-container JSON model nesting limit before changing
  parser state; reject a 513th nested array or object and invalid stack-growth
  overrides before allocation. Preserve parser behavior
  through dedicated Unicode, streaming, malformed-input and allocator tests,
  and remove the vendored parser's blanket lint suppression.
