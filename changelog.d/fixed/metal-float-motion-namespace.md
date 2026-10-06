- **The macOS Metal build compiles again.** `float_motion_metal.mm` opened an
  anonymous namespace it never closed, and every macOS Metal build stopped
  there. A new device-free test checks the braces and namespaces of every
  Metal host file on every platform.
