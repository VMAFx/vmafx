- **The toolchain probe now pairs the MinGW cross runtime with the compiler's thread model, not only its
  presence (#3307).** The new `x86_64-w64-mingw32-gcc` entry prints the driver's `Thread model:` line, resolves
  `libstdc++-6.dll` / `libgcc_s_seh-1.dll` with `gcc -print-file-name` (never a path rebuilt from `-dumpversion`,
  which Debian's flavoured drivers report as `13-win32`), exits non-zero when a resolution fails, and
  `objdump -p`-asserts the runtime's import table against the detected model — a win32-model driver next to a
  `libwinpthread`-importing runtime refuses instead of pasting green evidence. The probe reports tool absence as
  exit 0; only a found driver whose runtime cannot be resolved or whose flavour contradicts the model fails.
Contributed by luoye-cpu.
