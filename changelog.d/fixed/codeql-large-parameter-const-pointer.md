Pass large structs (VifBuffer, SpeedDimensions, SpeedInternalDimensions, CambiBuffers) by
const-pointer instead of by value to resolve 30 CodeQL cpp/large-parameter alerts.
Bit-exactness is preserved: this is a pure calling-convention change with no arithmetic
impact, confirmed by the 88/88 fast-suite gate including the Netflix CPU golden assertions.
