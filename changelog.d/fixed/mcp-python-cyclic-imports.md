- **mcp-server**: removed the CodeQL `py/cyclic-import` pair between the Python
  stdio server and HTTP transport.  HTTP scoring now crosses the acyclic
  `http_scoring` interface, while request validation, execution, and strict JSON
  behavior remain owned by the canonical server implementation.  Embedded HTTP
  launchers may inject a per-server adapter without mutating process-global
  policy; missing adapters fail before socket binding. The default development
  dependencies now include the metrics client so HTTP regression tests execute
  in CI rather than silently skipping at collection.
