Give the SpEED chroma and temporal SYCL kernels distinct linker identities so
Intel Arc runs no longer pair a host capture layout with the other twin's
device image, restore the explicit moment-kernel output capture lost by an
unrelated change, and guard both contracts alongside the fp64-free device
regions.
