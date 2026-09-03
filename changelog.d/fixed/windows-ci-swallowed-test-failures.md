- Fixed the Windows CI test step silently passing when any test other than the
  last one in its list failed. GitHub runs `shell: cmd` with `/V:OFF`, so the
  step reported only the final executable's exit code; each test is now
  checked with `|| exit /b 1`. `test_output` was also added to that list.
