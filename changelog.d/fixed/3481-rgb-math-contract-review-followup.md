- **Forward `main()` argv and pin timeouts in `test_vmafx_rgb_math_contract`.**
  `main(argv)` called `unittest.main()` without `argv`, parsing `sys.argv`
  instead of its own argument list; it now passes `argv=[sys.argv[0], *args]`,
  and the warm-up test pins `timeout=TIMEOUT` on every compiler call.
