- Fixed the MCP HTTP body-limit regression on Python 3.14 with aiohttp 3.14.3:
  the test client now streams its 4 MiB + 1 request through `io.BytesIO`, so
  warnings-as-errors no longer abort before the request can assert HTTP 413.
