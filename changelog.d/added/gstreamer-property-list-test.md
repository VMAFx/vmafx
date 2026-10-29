- **The `vmafx` GStreamer element's properties are tested against the API definition (#2474).**
  `gstreamer/test/test_gst_vmafx_properties.py` compares the properties the
  element installs, and the property table of the GStreamer user page, with the
  options of `core/api/vmafx.toml`; a planted extra or missing property fails.
