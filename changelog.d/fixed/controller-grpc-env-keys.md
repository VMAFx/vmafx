- **`VMAFX_GRPC_CERT_FILE`, `VMAFX_GRPC_KEY_FILE`,
  `VMAFX_GRPC_MAX_RECV_SIZE` and `VMAFX_GRPC_MAX_SEND_SIZE` reach the
  controller's gRPC server.** `vmafx-controller` did not declare these keys
  as golusoris CompoundKeys, so the variables became `grpc.cert.file` and
  similar keys nothing reads: the size limits stayed at the framework's
  4 MiB, and `VMAFX_GRPC_TLS=true` stopped the controller at startup with an
  empty certificate path. The controller's list is now generated with the
  other binaries' ([ADR-2350](docs/adr/2350-cloud-native-platform.md)).
