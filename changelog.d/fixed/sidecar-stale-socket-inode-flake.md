- Tests: the sidecar test that replaces a stale socket no longer fails at
  random on ext4. It now keeps a second link to the stale socket, so the file
  system cannot give its inode number to the server's new socket
  (`ai/sidecar/tests/test_socket_permissions.py`).
