- Align the CUDA stack on 13.4.1 across digest-pinned images, the dev
  container, Linux and Windows CI, documentation, and the native Windows ARM64
  build. CUDA installers now consume `build-config.env`; the ARM64 lane also
  compiles the CUDA backend with an ARM64 MSVC host compiler.
- Keep workflow lint and impact planning fail-closed for the new Windows ARM64
  runner label and previously unclassified governance/editor surfaces.
