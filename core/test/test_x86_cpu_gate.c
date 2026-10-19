/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Copyright 2026 Lusoris
 *
 * ADR-2055: the AVX2 level of vmaf_get_cpu_flags_x86() means AVX2 + FMA +
 * BMI1/BMI2. Two AVX2 kernels (ms_ssim_decimate_avx2, ssimulacra2 linear RGB)
 * are compiled with -mfma, so a processor or hypervisor that reports AVX2
 * without FMA must fall to the SSE levels instead of faulting on them.
 *
 * x86/cpu.c is compiled into this test with mock vmaf_cpu_cpuid() and
 * vmaf_cpu_xgetbv() in place of cpuid.asm, so each case chooses the CPUID
 * leaves the gate reads.
 */

#include <stdint.h>
#include <string.h>

#include "test.h"
#include "x86/cpu.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C mode has no `nullptr`
 * (ADR-1138). */

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} CpuidRegisters;

/* Leaf 1 ECX: SSE3 | SSSE3 (0x201), SSE4.1 (0x80000), FMA (bit 12), OSXSAVE | AVX (0x18000000). */
#define LEAF1_ECX_BASE 0x18080201u
#define LEAF1_ECX_FMA 0x00001000u
#define LEAF1_EDX_SSE2 0x06008000u
/* Leaf 7 EBX: BMI1 | BMI2 | AVX2. */
#define LEAF7_EBX_AVX2 0x00000128u
/* Leaf 7 EBX: AVX512F | DQ | CD | BW | VL (0xd0030000). */
#define LEAF7_EBX_AVX512 0xd0030000u

static struct {
    uint32_t leaf1_ecx;
    uint32_t leaf7_ebx;
    uint64_t xcr0;
} fake;

// NOLINTBEGIN(misc-use-internal-linkage): prototypes of the cpuid.asm symbols x86/cpu.c calls (ADR-2055)
void vmaf_cpu_cpuid(CpuidRegisters *regs, unsigned leaf, unsigned subleaf);
uint64_t vmaf_cpu_xgetbv(unsigned xcr);
// NOLINTEND(misc-use-internal-linkage)

/* The two functions x86/cpu.c declares extern and cuts out of cpuid.asm; this test links
 * x86/cpu.c itself, so these definitions are used from another translation unit
 * (ADR-2055). */
// NOLINTNEXTLINE(misc-use-internal-linkage): replaces the cpuid.asm symbol x86/cpu.c calls (ADR-2055)
void vmaf_cpu_cpuid(CpuidRegisters *regs, unsigned leaf, unsigned subleaf)
{
    (void)subleaf;
    memset(regs, 0, sizeof(*regs));
    if (leaf == 0) {
        regs->eax = 7;
    } else if (leaf == 1) {
        regs->edx = LEAF1_EDX_SSE2;
        regs->ecx = fake.leaf1_ecx;
    } else if (leaf == 7) {
        regs->ebx = fake.leaf7_ebx;
    }
}

// NOLINTNEXTLINE(misc-use-internal-linkage): replaces the cpuid.asm symbol x86/cpu.c calls (ADR-2055)
uint64_t vmaf_cpu_xgetbv(unsigned xcr)
{
    (void)xcr;
    return fake.xcr0;
}

static unsigned flags_for(uint32_t leaf1_ecx, uint32_t leaf7_ebx, uint64_t xcr0)
{
    fake.leaf1_ecx = leaf1_ecx;
    fake.leaf7_ebx = leaf7_ebx;
    fake.xcr0 = xcr0;
    return vmaf_get_cpu_flags_x86();
}

/* Positive: AVX2 + FMA + BMI1/2 sets the AVX2 level. */
static char *test_avx2_with_fma_sets_the_level(void)
{
    const unsigned flags = flags_for(LEAF1_ECX_BASE | LEAF1_ECX_FMA, LEAF7_EBX_AVX2, 0x6);
    mu_assert("AVX2 + FMA must set VMAF_X86_CPU_FLAG_AVX2", flags & VMAF_X86_CPU_FLAG_AVX2);
    return NULL;
}

/* Negative: AVX2 without FMA falls to the next lower level (SSE4.1), and AVX-512 stays off. */
static char *test_avx2_without_fma_falls_to_sse(void)
{
    const unsigned flags = flags_for(LEAF1_ECX_BASE, LEAF7_EBX_AVX2 | LEAF7_EBX_AVX512, 0xe6);
    mu_assert("AVX2 without FMA must not set VMAF_X86_CPU_FLAG_AVX2",
              !(flags & VMAF_X86_CPU_FLAG_AVX2));
    mu_assert("AVX-512 without FMA must not be reported", !(flags & VMAF_X86_CPU_FLAG_AVX512));
    mu_assert("the lower levels stay", (flags & VMAF_X86_CPU_FLAG_SSE41) != 0);
    return NULL;
}

/* Boundary: FMA alone (no AVX2 bits in leaf 7) and a missing BMI2 do not set the level. */
static char *test_fma_alone_and_missing_bmi2_do_not_set_the_level(void)
{
    unsigned flags = flags_for(LEAF1_ECX_BASE | LEAF1_ECX_FMA, 0, 0x6);
    mu_assert("FMA alone must not set AVX2", !(flags & VMAF_X86_CPU_FLAG_AVX2));
    flags = flags_for(LEAF1_ECX_BASE | LEAF1_ECX_FMA, LEAF7_EBX_AVX2 & ~0x100u, 0x6);
    mu_assert("AVX2 without BMI2 must not set AVX2", !(flags & VMAF_X86_CPU_FLAG_AVX2));
    return NULL;
}

/* AVX-512 keeps working on top of AVX2 + FMA. */
static char *test_avx512_needs_the_avx2_level(void)
{
    const unsigned flags =
        flags_for(LEAF1_ECX_BASE | LEAF1_ECX_FMA, LEAF7_EBX_AVX2 | LEAF7_EBX_AVX512, 0xe6);
    mu_assert("AVX-512 with FMA must be reported", flags & VMAF_X86_CPU_FLAG_AVX512);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_avx2_with_fma_sets_the_level);
    mu_run_test(test_avx2_without_fma_falls_to_sse);
    mu_run_test(test_fma_alone_and_missing_bmi2_do_not_set_the_level);
    mu_run_test(test_avx512_needs_the_avx2_level);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
