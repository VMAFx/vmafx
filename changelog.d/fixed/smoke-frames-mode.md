- **Make published node image smoke-test frames readable.** The FUSE frames
  folder was created by `mktemp -d` with mode 0700, preventing the nonroot mount
  container from reading it and causing every `vmafx-node` and
  `vmafx-node-...-sycl` publish smoke step to fail. The folder is now 0755, and
  the test stub refuses unreadable folders.
