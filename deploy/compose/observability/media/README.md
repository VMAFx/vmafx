# Media for the Compose example

Put the reference and distorted videos to score here (Y4M or any container
the bundled FFmpeg reads). The directory is mounted read-only at `/media` in
`vmafx-server`, `vmafx-controller` and `vmafx-node`, and `/media` is the only
scoring root, so request paths start with `/media/`. `VMAFX_MEDIA_DIR` names
another directory.
