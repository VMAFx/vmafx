- **The Go services build on golusoris v0.16.0** (from v0.15.0; core stays
  v0.10.1). The release carries a security fix in `container/registry`
  (golusoris#742): a tampered blob in an OCI image layout now fails with
  `ErrDigestMismatch` before its last bytes leave the process. Previously the
  whole blob could be uploaded, leaving the rejection to the registry. VMAFx
  does not import `container/registry` yet, so no shipped binary was exposed;
  the artifact work of #2431 picks the fixed module up with its first import.
  The release also adds artifact deletion with layout garbage collection and
  AWS, GCP and Azure KMS signers, which VMAFx does not use yet. Nothing VMAFx
  runs today changes: none of the golusoris packages VMAFx imports changed.
