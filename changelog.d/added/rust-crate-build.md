<!-- Copyright 2026 Lusoris -->
<!-- SPDX-License-Identifier: EUPL-1.2 -->
<!-- markdownlint-disable MD041 -->

- Rust bindings discover complete libvmaf installations through
  `LIBVMAF_PREFIX`, `/usr/local` and `/usr`, with a named missing-library
  error and a header-only `DOCS_RS` mode for source checkouts. The crate
  package has an explicit source allowlist. New Rust installation and Go
  module guides explain build requirements, fork tags and commit pins.
