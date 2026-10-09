- **The Go services build on golusoris v0.14.0** (from v0.13.1; core stays
  v0.10.1). The release adds paged object listings (`storage.Walk`,
  `ListOptions.StartAfter`) and in-process image signing and verification
  (`container/registry/sign`), which the object-storage and artifact work of
  the cloud-native platform (#2431) will use. Nothing VMAFx runs today changes:
  of the golusoris packages VMAFx imports, only `db/migrate` changed, in its
  file-path source, and VMAFx migrates from an embedded file system.
