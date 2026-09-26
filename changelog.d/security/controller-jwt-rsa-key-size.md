- **Controller authentication**: `vmafx-controller` now ignores JWKS signing
  keys shorter than 2048 bits and rejects RSA public exponents that are even,
  below 3 or out of range. Tokens signed with a skipped key are refused with
  `401`; other keys in the same JWKS keep working. Previously any key Go
  accepts, down to 1024 bits, could authenticate.
