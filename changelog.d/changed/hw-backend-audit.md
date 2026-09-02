docs(research): hardware backend audit recommends dropping Vulkan backend (#733)

Research digest 0733 audits all six GPU backends (CUDA, HIP, SYCL, Vulkan, Metal)
against the post-rebrand container-first k8s deployment model. Recommendation: drop
Vulkan (30 135 LOC, 3 unresolved bugs, no k8s-native representation); keep CUDA, HIP,
SYCL, and Metal. All vendors retain native GPU coverage after the drop.
