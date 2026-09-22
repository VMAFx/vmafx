import ctypes


# ctypes.memmove copies between raw addresses with no bounds check and no recorded
# rationale. It is genuine unsafe memory dereferencing inside a memory-safe language,
# and nothing implements a Python HISS-09 check.
def copy(dst_address, src_address, count):
    ctypes.memmove(dst_address, src_address, count)
