- **The Go services build on golusoris v0.15.0** (from v0.14.0; core stays
  v0.10.1). The release adds offline signing and verification of OCI
  image-layout directories, copies between registries and layouts that carry
  signatures along, OIDC token sources for keyless signing (GitHub Actions and
  a projected token file), and a Vault and OpenBao transit-key signer. The
  artifact and object-storage work of the cloud-native platform (#2431) will
  use them. Nothing VMAFx runs today changes: none of the golusoris packages
  VMAFx imports changed.
