/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  vulkan_probe.c — device self-check probe (ADR-3402). Port of the
 *  PoC Phase 0 (`poc/vk-integer-adm` phase0/src/vk_probe.c) onto volk,
 *  with identity-based device selection and JSON output.
 *
 *  All four checks and the verdict logic are the PoC's; the dynlink
 *  scaffolding is replaced by volk (the same loader the runtime uses)
 *  and the SPIR-V arrives through the build-time spv_embed pipeline
 *  (probe_spv.h) instead of a file read.
 */

#include "vulkan_probe.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#include "probe_spv.h" /* build-time embedded SPIR-V (spv_embed.py) */

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t lo, hi;
} u64pair;

static u64pair host_umul32(uint32_t a, uint32_t b)
{
    const uint64_t p = (uint64_t)a * (uint64_t)b;
    const u64pair r = {(uint32_t)(p & 0xffffffffu), (uint32_t)(p >> 32)};
    return r;
}

static inline float u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* Deterministic operand stream with high bit patterns set (the PoC's
 * "VKPC" seed). Kept byte-identical so probe runs are comparable with
 * the PoC reports. */
static void fill_operands(uint32_t *in0, uint32_t *in1, uint32_t count)
{
    srand(0x564b5043u); /* "VKPC" */
    for (uint32_t i = 0; i < count; i++) {
        in0[i] = (((uint32_t)rand() << 17) ^ (uint32_t)rand()) | 1u;
        in1[i] = (((uint32_t)rand() << 13) ^ (uint32_t)rand()) | 1u;
    }
}

/* ------------------------------------------------------------------ */
/* Probe.                                                              */
/* ------------------------------------------------------------------ */

int vmaf_vulkan_probe_run(const VmafVulkanProbeParams *params, VmafVulkanProbeResult *result)
{
    if (!params || !result)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    FILE *out = params->out ? params->out : stderr;

    const uint32_t count = params->count ? params->count : 4096u;
    if (count == 0 || (count % 64u) != 0)
        return -EINVAL;

    /* ---- loader ---- */
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(out, "{\"probe\":\"vmafx-vk-probe\",\"error\":\"no Vulkan loader\"}\n");
        return -ENODEV;
    }

    /* ---- instance (1.3, falling back like the PoC) ---- */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "vmafx-vk-probe",
        .applicationVersion = 1,
    };
    static const uint32_t want_api[] = {VK_MAKE_API_VERSION(0, 1, 3, 0),
                                        VK_MAKE_API_VERSION(0, 1, 1, 0),
                                        VK_MAKE_API_VERSION(0, 1, 0, 0)};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INCOMPATIBLE_DRIVER;
    for (unsigned i = 0; i < 3; i++) {
        app.apiVersion = want_api[i];
        VkInstanceCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app,
        };
        r = vkCreateInstance(&ici, NULL, &instance);
        if (r == VK_SUCCESS)
            break;
    }
    if (r != VK_SUCCESS) {
        fprintf(out, "{\"probe\":\"vmafx-vk-probe\",\"error\":\"vkCreateInstance failed (%d)\"}\n",
                (int)r);
        return -ENODEV;
    }
    volkLoadInstance(instance);

    /* ---- enumerate + identity selection ---- */
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(instance, &ndev, NULL);
    if (ndev == 0) {
        fprintf(out, "{\"probe\":\"vmafx-vk-probe\",\"error\":\"no Vulkan physical devices\"}\n");
        vkDestroyInstance(instance, NULL);
        return -ENODEV;
    }
    VkPhysicalDevice *devs = malloc(ndev * sizeof(*devs));
    if (!devs) {
        vkDestroyInstance(instance, NULL);
        return -ENOMEM;
    }
    vkEnumeratePhysicalDevices(instance, &ndev, devs);

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props = {0};
    for (uint32_t i = 0; i < ndev && i < 8; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        snprintf(result->adapters[result->adapters_seen],
                 sizeof(result->adapters[0]), "%s vendor=0x%04x device=0x%04x", p.deviceName,
                 p.vendorID, p.deviceID);
        result->adapters_seen++;

        const int identity_requested =
            (params->vendor_id != 0u || params->device_id != 0u);
        if (phys != VK_NULL_HANDLE)
            continue;
        if (identity_requested) {
            if (p.vendorID == params->vendor_id && p.deviceID == params->device_id)
                phys = devs[i];
        } else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys = devs[i];
        }
        if (phys == VK_NULL_HANDLE && !identity_requested && (i + 1u) == ndev)
            phys = devs[0]; /* auto fallback: first adapter */
        if (phys != VK_NULL_HANDLE)
            props = p;
    }

    if (phys == VK_NULL_HANDLE) {
        fprintf(out,
                "{\"probe\":\"vmafx-vk-probe\",\"error\":\"requested identity not selectable\","
                "\"requested\":{\"vendorID\":\"0x%04x\",\"deviceID\":\"0x%04x\"},"
                "\"adapters_seen\":[",
                params->vendor_id, params->device_id);
        for (unsigned i = 0; i < result->adapters_seen; i++)
            fprintf(out, "%s\"%s\"", i ? "," : "", result->adapters[i]);
        fprintf(out, "]}\n");
        free(devs);
        vkDestroyInstance(instance, NULL);
        return -ENODEV;
    }

    snprintf(result->device_name, sizeof(result->device_name), "%s", props.deviceName);
    result->vendor_id = props.vendorID;
    result->device_id = props.deviceID;

    /* Device-reported features. The probe requests ONLY what the query
     * answered VK_TRUE (the Intel vkCreateDevice SIGSEGV, #2466 comment
     * 6689054122). Only shaderInt64 is ever requested (op1 needs it);
     * op2 needs no fp64. */
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    uint32_t compute_family = UINT32_MAX;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof(*qp));
    if (!qp) {
        free(devs);
        vkDestroyInstance(instance, NULL);
        return -ENOMEM;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qp);
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            compute_family = i;
            break;
        }
    if (compute_family == UINT32_MAX) {
        fprintf(out, "{\"probe\":\"vmafx-vk-probe\",\"error\":\"no compute queue\"}\n");
        free(qp);
        free(devs);
        vkDestroyInstance(instance, NULL);
        return -ENODEV;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = compute_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkPhysicalDeviceFeatures enabled = {0};
    if (f2.features.shaderInt64)
        enabled.shaderInt64 = VK_TRUE;
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .pEnabledFeatures = &enabled,
    };
    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, NULL, &device);
    if (r != VK_SUCCESS) {
        fprintf(out, "{\"probe\":\"vmafx-vk-probe\",\"error\":\"vkCreateDevice failed (%d)\"}\n",
                (int)r);
        free(qp);
        free(devs);
        vkDestroyInstance(instance, NULL);
        return -ENODEV;
    }
    volkLoadDevice(device);

    VkQueue queue;
    vkGetDeviceQueue(device, compute_family, 0, &queue);

    /* ---- resources: 4 host-visible storage buffers ---- */
    /* Every handle the cleanup paths touch is declared up front and
     * zero-initialised: the error paths below `goto` over the later
     * declarations, and a jumped-over declaration would leave the
     * cleanup reading indeterminate handles. */
    enum { B_IN0, B_IN1, B_OUT0, B_OUT1, NBUF };
    VkBuffer bufs[NBUF] = {0};
    VkDeviceMemory mems[NBUF] = {0};
    uint32_t *mapped[NBUF] = {0};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t *in0 = NULL;
    uint32_t *in1 = NULL;
    VkPhysicalDeviceMemoryProperties memprops;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    for (int b = 0; b < NBUF; b++) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = (VkDeviceSize)count * sizeof(uint32_t),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(device, &bci, NULL, &bufs[b]) != VK_SUCCESS)
            goto fail_resources;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, bufs[b], &mr);
        uint32_t mtype = 0xffffffffu;
        for (uint32_t m = 0; m < memprops.memoryTypeCount; m++) {
            if ((mr.memoryTypeBits & (1u << m)) &&
                (memprops.memoryTypes[m].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                mtype = m;
                break;
            }
        }
        if (mtype == 0xffffffffu)
            goto fail_resources;
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mtype,
        };
        if (vkAllocateMemory(device, &mai, NULL, &mems[b]) != VK_SUCCESS)
            goto fail_resources;
        if (vkBindBufferMemory(device, bufs[b], mems[b], 0) != VK_SUCCESS)
            goto fail_resources;
        if (vkMapMemory(device, mems[b], 0, mr.size, 0, (void **)&mapped[b]) != VK_SUCCESS)
            goto fail_resources;
        memset(mapped[b], 0, (size_t)mr.size);
    }

    /* ---- descriptors ---- */
    VkDescriptorSetLayoutBinding bindings[NBUF];
    for (int b = 0; b < NBUF; b++) {
        bindings[b] = (VkDescriptorSetLayoutBinding){
            .binding = (uint32_t)b,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
    }
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = NBUF,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(device, &dlci, NULL, &set_layout) != VK_SUCCESS)
        goto fail_resources;
    VkDescriptorPoolSize ps = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = NBUF,
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    if (vkCreateDescriptorPool(device, &pci, NULL, &pool) != VK_SUCCESS)
        goto fail_resources;
    VkDescriptorSetAllocateInfo sai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    if (vkAllocateDescriptorSets(device, &sai, &set) != VK_SUCCESS)
        goto fail_resources;
    VkWriteDescriptorSet writes[NBUF];
    VkDescriptorBufferInfo binfos[NBUF];
    for (int b = 0; b < NBUF; b++) {
        binfos[b] = (VkDescriptorBufferInfo){
            .buffer = bufs[b],
            .offset = 0,
            .range = (VkDeviceSize)count * sizeof(uint32_t),
        };
        writes[b] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = (uint32_t)b,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &binfos[b],
        };
    }
    vkUpdateDescriptorSets(device, NBUF, writes, 0, NULL);

    /* ---- pipeline ---- */
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 16,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(device, &plci, NULL, &pipe_layout) != VK_SUCCESS)
        goto fail_resources;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = probe_spv_size,
        .pCode = probe_spv,
    };
    if (vkCreateShaderModule(device, &smci, NULL, &module) != VK_SUCCESS)
        goto fail_resources;
    VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module,
        .pName = "main",
    };
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = pipe_layout,
    };
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline) != VK_SUCCESS)
        goto fail_resources;

    VkCommandPoolCreateInfo cpoi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = compute_family,
    };
    if (vkCreateCommandPool(device, &cpoi, NULL, &cmd_pool) != VK_SUCCESS)
        goto fail_resources;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS)
        goto fail_resources;
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    if (vkCreateFence(device, &fci, NULL, &fence) != VK_SUCCESS)
        goto fail_resources;

    /* ---- per-op run + verify (the PoC's logic, verbatim where it is
     * verdict-bearing) ---- */
    static const struct {
        uint32_t op;
        const char *name;
    } ops[] = {
        {0, "umul32-product"},
        {1, "u64-add-reduce"},
        {2, "fma-contraction"},
        {3, "dispatch-sentinel"},
    };
    in0 = malloc((size_t)count * 4);
    in1 = malloc((size_t)count * 4);
    if (!in0 || !in1)
        goto fail_run;
    fill_operands(in0, in1, count);

    fprintf(out, "{\"probe\":\"vmafx-vk-probe\",\"device\":\"%s\",\"vendorID\":\"0x%04x\","
                 "\"deviceID\":\"0x%04x\",\"api\":\"%u.%u.%u\",\"driver\":\"%u.%u.%u\","
                 "\"shaderFloat64\":%d,\"shaderInt64\":%d,\"count\":%u,\"checks\":[\n",
            props.deviceName, props.vendorID, props.deviceID, props.apiVersion >> 22,
            (props.apiVersion >> 12) & 0x3ffu, props.apiVersion & 0xfffu,
            props.driverVersion >> 22, (props.driverVersion >> 14) & 0x3ffu,
            props.driverVersion & 0x3fffu, (int)f2.features.shaderFloat64,
            (int)f2.features.shaderInt64, count);

    int all_pass = 1;
    for (size_t oi = 0; oi < sizeof(ops) / sizeof(ops[0]); oi++) {
        const uint32_t op = ops[oi].op;
        for (uint32_t i = 0; i < count; i++) {
            mapped[B_IN0][i] = in0[i];
            mapped[B_IN1][i] = in1[i];
        }
        uint32_t pc[4] = {op, count, 0x12345678u ^ (op * 0x9e3779b9u), 0};
        if (op == 2) {
            /* a*b+c where contraction matters: the product is
             * mantissa-heavy, c = 1.0f rounds the product's low bits
             * away in mul+add but FMA keeps them (the PoC's operand
             * choice). */
            pc[3] = 0x3f800000u;
        }
        VkCommandBufferBeginInfo bbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        vkBeginCommandBuffer(cmd, &bbi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &set, 0,
                                NULL);
        vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
        const uint32_t groups = (op == 1u) ? 1u : count / 64u;
        vkCmdDispatch(cmd, groups, 1, 1);
        vkEndCommandBuffer(cmd);
        vkResetFences(device, 1, &fence);
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
        };
        int pass = 0;
        if (vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS &&
            vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000ull * 1000ull * 1000ull) ==
                VK_SUCCESS)
            pass = 1;

        const char *verdict = "FAIL";
        if (op == 0) {
            int ok = pass;
            for (uint32_t i = 0; ok && i < count; i++) {
                const u64pair g = host_umul32(in0[i], in1[i]);
                const uint32_t lo = mapped[B_OUT0][i] ^ pc[2];
                const uint32_t hi = mapped[B_OUT1][i] ^ pc[2];
                if (lo != g.lo || hi != g.hi)
                    ok = 0;
            }
            result->check_umul32 = ok ? 1 : 0;
            verdict = ok ? "PASS" : "FAIL";
        } else if (op == 1) {
            /* The carry-chain reduce is 32-bit arithmetic with explicit
             * carries (probe.comp op1) — it exercises the u64-carry
             * pattern the emulated 64-bit reductions use without
             * needing the shaderInt64 feature, so it runs on every
             * compute-capable device. */
            int ok = pass;
            for (uint32_t i = 0; ok && i < count / 64u; i++) {
                const uint32_t base = i * 64u;
                uint32_t lo = in0[base], hi = in1[base];
                for (uint32_t k = 1u; k < 64u; k++) {
                    const uint32_t nlo = in0[base + k], nhi = in1[base + k];
                    const uint32_t t = lo + nlo;
                    const uint32_t carry = (t < lo) ? 1u : 0u;
                    hi = hi + nhi + carry;
                    lo = t;
                }
                const uint32_t glo = mapped[B_OUT0][i] ^ pc[2];
                const uint32_t ghi = mapped[B_OUT1][i] ^ pc[2];
                if (lo != glo || hi != ghi)
                    ok = 0;
            }
            result->check_u64_reduce = ok ? 1 : 0;
            verdict = ok ? "PASS" : "FAIL";
        } else if (op == 2) {
            const float c = u2f(pc[3]);
            uint32_t fma_hits = 0, sep_hits = 0, edge = 0, other = 0, sep_violation = 0;
            for (uint32_t i = 0; i < count; i++) {
                const float a = u2f(in0[i]), b = u2f(in1[i]);
                const float ref_add = a * b + c;     /* host: two roundings (-ffp-contract=off) */
                const float ref_fma = fmaf(a, b, c); /* host: fused */
                const uint32_t got_c = mapped[B_OUT0][i] ^ pc[2];
                const uint32_t got_s = mapped[B_OUT1][i] ^ pc[2];
                /* NaN payloads, denormal inputs (DAZ) and denormal
                 * outputs (FTZ) differ per implementation; bucket them
                 * as edge behaviour instead of failure (the PoC's
                 * bucketing; DAZ is an input assumption per ADR-3402). */
                const int nonfinite = !isfinite(a) || !isfinite(b) || !isfinite(c) ||
                                      !isfinite(ref_add) || !isfinite(ref_fma);
                const int denormal_in =
                    (a != 0.0f && fabsf(a) < FLT_MIN) || (b != 0.0f && fabsf(b) < FLT_MIN) ||
                    (c != 0.0f && fabsf(c) < FLT_MIN);
                if (nonfinite || denormal_in) {
                    edge++;
                    continue;
                }
                if (got_c == f2u(ref_fma)) {
                    fma_hits++;
                } else if (got_c == f2u(ref_add)) {
                    sep_hits++;
                } else {
                    other++;
                }
                if (got_s == f2u(ref_add)) {
                    sep_hits++;
                } else if (got_s == f2u(ref_fma)) {
                    fma_hits++;
                    sep_violation++;
                } else {
                    other++;
                }
            }
            result->contraction_violations = sep_violation;
            result->contraction_unclassified = other;
            /* separated (precise) must equal mul+add for every finite
             * case: that is NoContraction being honored; other > 0 is
             * unclassifiable. */
            const int ok = pass && (other == 0 && sep_violation == 0);
            result->check_contraction = ok ? 1 : 0;
            verdict = ok ? "PASS" : "FAIL";
        } else {
            int ok = pass;
            for (uint32_t i = 0; ok && i < count; i++) {
                if (mapped[B_OUT0][i] != ((i * 2654435761u) ^ pc[2]) ||
                    mapped[B_OUT1][i] != (0xDEADBEEFu ^ (i + pc[2])))
                    ok = 0;
            }
            result->check_sentinel = ok ? 1 : 0;
            verdict = ok ? "PASS" : "FAIL";
        }

        fprintf(out,
                "{\"op\":%u,\"name\":\"%s\",\"verdict\":\"%s\",\"precise_violations\":%u,"
                "\"unclassified\":%u}%s\n",
                op, ops[oi].name, verdict,
                op == 2 ? result->contraction_violations : 0u,
                op == 2 ? result->contraction_unclassified : 0u,
                (oi + 1u) == sizeof(ops) / sizeof(ops[0]) ? "" : ",");
        if (strcmp(verdict, "PASS") != 0)
            all_pass = 0;
    }
    fprintf(out, "]}\n");

    free(in0);
    free(in1);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, module, NULL);
    vkDestroyPipelineLayout(device, pipe_layout, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (int b = 0; b < NBUF; b++) {
        if (mapped[b])
            vkUnmapMemory(device, mems[b]);
        if (mems[b])
            vkFreeMemory(device, mems[b], NULL);
        if (bufs[b])
            vkDestroyBuffer(device, bufs[b], NULL);
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(qp);
    free(devs);

    result->err = all_pass ? 0 : -ENOTSUP;
    return result->err;

fail_run:
    free(in0);
    free(in1);
fail_resources:
    if (fence != VK_NULL_HANDLE)
        vkDestroyFence(device, fence, NULL);
    if (cmd != VK_NULL_HANDLE)
        vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);
    if (cmd_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(device, cmd_pool, NULL);
    if (pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, pipeline, NULL);
    if (module != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, module, NULL);
    if (pipe_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, pipe_layout, NULL);
    if (pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device, pool, NULL);
    if (set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (int b = 0; b < NBUF; b++) {
        if (mapped[b])
            vkUnmapMemory(device, mems[b]);
        if (mems[b])
            vkFreeMemory(device, mems[b], NULL);
        if (bufs[b])
            vkDestroyBuffer(device, bufs[b], NULL);
    }
    if (device != VK_NULL_HANDLE)
        vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(qp);
    free(devs);
    return -ENOMEM;
}
