- **The Windows SYCL build compiles again.** Since the math constants come
  from `<math.h>` (#2638), the SYCL feature sources need `_USE_MATH_DEFINES`
  on Windows, which the project-wide argument did not reach: icpx compiles
  them in custom targets. Both SYCL argument lists now carry the define, and
  `test_sycl_math_constants_contract.py` keeps every icpx compile line on it.
