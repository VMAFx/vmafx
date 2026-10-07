/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * test_pelorus_interop.c — vmafx side of the SHARED Pelorus interop ABI
 * conformance fixture (VMAFx/pelorus@11e183ec0aedf6b3e6447fda64acbb6072a1ae60
 * test/interop_test.c, ABI 1.3).
 *
 * Both repos run byte-for-byte the same checks against their own copy of
 * interop.c. A green run here proves vmafx's vendored parser (ADR-1113) is
 * byte-identical to pelorus's writer: a blob packed by pelorus parses in vmafx
 * and vice versa, and the forward/back-compat rules (R3, R4, R6) hold. Keep
 * this file in sync with the pelorus original via
 * scripts/sync-pelorus-interop.sh; the only intended local edit is the include
 * path rewrite ("pelorus/<x>.h" -> "libvmaf/pelorus/<x>.h").
 *
 * No external test framework — exit non-zero on first failure (does NOT use the
 * libvmaf minunit harness in test.c; it carries its own main()).
 */

#include "libvmaf/pelorus/deband.h"
#include "libvmaf/pelorus/interop.h"
#include "libvmaf/pelorus/pelorus.h"

/* NOLINTBEGIN(modernize-use-nullptr): this C translation unit is built as C23
 * by vmafx, where clang-tidy also proposes the `nullptr` keyword, but MSVC's C
 * mode has no `nullptr` (C2065); the Windows builds compile it with cl.exe.
 * The NULL macro stays. Same decision as vmafx ADR-1138
 * (docs/adr/1138-c-translation-units-keep-null.md in VMAFx/vmafx). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
/* windows.h first: sddl.h relies on its types. */
#include <sddl.h>
#include <share.h>
#ifdef _MSC_VER
/* The fixture's Win32 security calls live in advapi32. */
#pragma comment(lib, "advapi32.lib")
#endif
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static int g_fail;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            g_fail++;                                                                              \
        }                                                                                          \
    } while (0)

/*
 * Fixture files (Pelorus issues #60 and #62, Pelorus ADR-0148). The fixture
 * writes files into the working directory, so it must never widen access or
 * write through a path it did not create:
 *   - creation is exclusive: an existing file, link, or dangling link at the
 *     path is refused, never followed, truncated, or replaced;
 *   - the file is owner-only from the first open: POSIX mode 0600 (the umask
 *     can only remove bits), Windows a protected DACL granting only the owner;
 *   - a file this code created is removed again on every failure path, and a
 *     path it refused is never touched.
 */
#define FIXTURE_MAX_BYTES 4096u

typedef enum {
    FIXTURE_CREATED,  /* created and fully written */
    FIXTURE_REFUSED,  /* nothing created: the path exists or creation failed */
    FIXTURE_ABANDONED /* created, then a write or close failed */
} fixture_status;

#ifdef _WIN32
/* Protected DACL, one ACE: full access for the file's owner (OWNER RIGHTS).
 * Nothing is inherited from the directory, the 0600 analogue. */
#define FIXTURE_OWNER_ONLY_SDDL "D:P(A;;FA;;;OW)"

static fixture_status fixture_write_new(const char *path, const char *contents, size_t len)
{
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    HANDLE file;
    DWORD written = 0;
    BOOL wrote;
    BOOL closed;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(FIXTURE_OWNER_ONLY_SDDL,
                                                              SDDL_REVISION_1, &sd, NULL)) {
        return FIXTURE_REFUSED;
    }
    sa.nLength = (DWORD)sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    /* CREATE_NEW alone follows a dangling link and creates its target;
     * FILE_FLAG_OPEN_REPARSE_POINT makes any existing link name fail. */
    file = CreateFileA(path, GENERIC_WRITE, 0, &sa, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    (void)LocalFree(sd);
    if (file == INVALID_HANDLE_VALUE) {
        return FIXTURE_REFUSED;
    }
    wrote = WriteFile(file, contents, (DWORD)len, &written, NULL);
    closed = CloseHandle(file);
    if (!wrote || !closed || written != (DWORD)len) {
        return FIXTURE_ABANDONED;
    }
    return FIXTURE_CREATED;
}

typedef union {
    SECURITY_DESCRIPTOR absolute; /* alignment for the self-relative copy */
    unsigned char bytes[1024];
} fixture_security_buf;

/* 1 when path is a regular file (not a link) with a present, non-NULL DACL,
 * read into buf; *control receives the descriptor's control flags. */
static int fixture_read_dacl(const char *path, fixture_security_buf *buf,
                             SECURITY_DESCRIPTOR_CONTROL *control, PACL *dacl)
{
    const DWORD attributes = GetFileAttributesA(path);
    DWORD need = 0;
    DWORD revision = 0;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;

    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !GetFileSecurityA(path, DACL_SECURITY_INFORMATION, buf, (DWORD)sizeof(*buf), &need) ||
        !GetSecurityDescriptorControl(buf, control, &revision) ||
        !GetSecurityDescriptorDacl(buf, &present, dacl, &defaulted)) {
        return 0;
    }
    return present && *dacl != NULL;
}

/* A regular file (not a link) whose DACL is protected and holds exactly one
 * allow ACE, for OWNER RIGHTS: no inherited, group, or world entry. */
static int fixture_is_owner_only(const char *path)
{
    fixture_security_buf buf;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    PACL dacl = NULL;
    void *ace = NULL;

    if (!fixture_read_dacl(path, &buf, &control, &dacl) || (control & SE_DACL_PROTECTED) == 0 ||
        dacl->AceCount != 1 || !GetAce(dacl, 0, &ace)) {
        return 0;
    }
    return ((const ACE_HEADER *)ace)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
           IsWellKnownSid(&((ACCESS_ALLOWED_ACE *)ace)->SidStart, WinCreatorOwnerRightsSid);
}

/* 0 created, 1 this account may not create links (skip), -1 error. Windows
 * needs Developer Mode or SeCreateSymbolicLinkPrivilege for file links. */
static int fixture_symlink(const char *target, const char *link_path)
{
    DWORD error;

    if (CreateSymbolicLinkA(link_path, target, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        return 0;
    }
    error = GetLastError();
    return (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_INVALID_PARAMETER) ? 1 : -1;
}
#else
static fixture_status fixture_write_new(const char *path, const char *contents, size_t len)
{
    /* O_EXCL fails on any existing name, links included (POSIX open());
     * O_NOFOLLOW states the same intent for the final component. */
    const int fd =
        open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    ssize_t written;
    int closed;

    if (fd < 0) {
        return FIXTURE_REFUSED;
    }
    written = write(fd, contents, len);
    closed = close(fd);
    if (written < 0 || (size_t)written != len || closed != 0) {
        return FIXTURE_ABANDONED;
    }
    return FIXTURE_CREATED;
}

/* A regular file (not a link) with exactly mode 0600. */
static int fixture_is_owner_only(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode) &&
           (st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) == (S_IRUSR | S_IWUSR);
}

/* 0 created, -1 error: every supported POSIX host can create links. */
static int fixture_symlink(const char *target, const char *link_path)
{
    return symlink(target, link_path) == 0 ? 0 : -1;
}
#endif

/* Create path exclusively and write contents. 0 on success. */
static int write_private_fixture(const char *path, const char *contents)
{
    const size_t len = strlen(contents);
    fixture_status status;

    if (len > FIXTURE_MAX_BYTES) {
        return -1;
    }
    status = fixture_write_new(path, contents, len);
    if (status == FIXTURE_ABANDONED) {
        CHECK(remove(path) == 0); /* ours: never leave a partial fixture behind */
    }
    return status == FIXTURE_CREATED ? 0 : -1;
}

/* Create a fixture under the most permissive umask, so only the requested
 * mode can keep it private, then prove it is owner-only. 0 on success; on
 * failure nothing this call created remains. */
static int create_checked_fixture(const char *path, const char *contents)
{
    int rc;
#ifndef _WIN32
    const mode_t old_umask = umask(0);
#endif

    rc = write_private_fixture(path, contents);
#ifndef _WIN32
    (void)umask(old_umask);
#endif
    if (rc != 0) {
        (void)fprintf(stderr,
                      "fixture %s: exclusive create refused (stale file from an aborted run?)\n",
                      path);
        return -1;
    }
    if (!fixture_is_owner_only(path)) {
        (void)fprintf(stderr, "fixture %s: group/world access or not a regular file\n", path);
        CHECK(remove(path) == 0);
        return -1;
    }
    return 0;
}

/* Opens a fixture for reading. Windows: _fsopen() with _SH_DENYNO, the sharing fopen() gives;
 * the CRT declares fopen() deprecated (C4996 under cl.exe, -Wdeprecated-declarations under
 * clang-cl and icx-cl), and fopen_s() would open the file exclusively. */
static FILE *fixture_open_read(const char *path)
{
#ifdef _WIN32
    return _fsopen(path, "rb", _SH_DENYNO);
#else
    return fopen(path, "rb");
#endif
}

/* The whole file equals expected: nothing truncated, appended, or replaced. */
static int fixture_equals(const char *path, const char *expected)
{
    char buf[64];
    size_t n;
    int closed;
    FILE *fp = fixture_open_read(path);

    if (fp == NULL) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf), fp);
    closed = fclose(fp);
    return closed == 0 && n == strlen(expected) && memcmp(buf, expected, n) == 0;
}

/* 1 when the path exists (checked by opening it, so a link is followed). */
static int fixture_path_exists(const char *path)
{
    FILE *fp = fixture_open_read(path);

    if (fp == NULL) {
        return 0;
    }
    CHECK(fclose(fp) == 0);
    return 1;
}

/* A link to an existing file is refused; the target keeps its bytes. Returns
 * 1 when this account cannot create links (Windows without the right). */
static int check_fixture_refuses_live_link(const char *target, const char *original)
{
    const char *live = "pelorus_fixture_link.tmp";
    const int link_rc = fixture_symlink(target, live);

    if (link_rc == 1) {
        (void)fprintf(stderr, "note: no symlink privilege; fixture link cases skipped\n");
        return 1;
    }
    CHECK(link_rc == 0);
    if (link_rc != 0) {
        return 0;
    }
    CHECK(write_private_fixture(live, "clobbered\n") != 0);
    CHECK(fixture_equals(target, original));
    CHECK(remove(live) == 0);
    return 0;
}

/* A dangling link is refused, and nothing is created at its target. */
static void check_fixture_refuses_dangling_link(void)
{
    const char *dangling = "pelorus_fixture_dangling.tmp";
    const char *absent = "pelorus_fixture_absent.tmp";
    const int stale = fixture_path_exists(absent);
    int link_rc;
    int created;

    CHECK(!stale); /* else the link would not dangle; never touch that file */
    if (stale) {
        return;
    }
    link_rc = fixture_symlink(absent, dangling);
    CHECK(link_rc == 0);
    if (link_rc != 0) {
        return;
    }
    CHECK(write_private_fixture(dangling, "clobbered\n") != 0);
    created = fixture_path_exists(absent);
    CHECK(!created); /* nothing may be created through the link */
    if (created) {
        CHECK(remove(absent) == 0);
    }
    CHECK(remove(dangling) == 0);
}

/* Pelorus issues #60 and #62: fixture creation never grants group/world
 * access and never follows, truncates, or replaces an existing path. */
static void test_fixture_file_safety(void)
{
    const char *plant = "pelorus_fixture_plant.tmp";
    const char *original = "planted\n";
    int rc;

    rc = create_checked_fixture(plant, original);
    CHECK(rc == 0);
    if (rc != 0) {
        return;
    }
    CHECK(write_private_fixture(plant, "clobbered\n") != 0);
    CHECK(fixture_equals(plant, original));
    if (check_fixture_refuses_live_link(plant, original) == 0) {
        check_fixture_refuses_dangling_link();
    }
    CHECK(remove(plant) == 0);
}

static void fill_meta(PelorusSideData *m)
{
    memset(m, 0, sizeof(*m));
    m->frame_pts = 123456;
    m->plane_layout = PEL_LAYOUT_420;
    m->bit_depth = 10;
    m->grid_cols = 16;
    m->grid_rows = 9;
    m->producer_id = PEL_FOURCC('P', 'L', 'R', 'S');
}

static void check_roundtrip_banding(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusBandingSection));
    {
        const PelorusBandingSection *b = p;
        CHECK(b->global_banding_risk == 0.42f);
        CHECK(b->flat_area_fraction == 0.61f);
    }
}

static void check_roundtrip_variance(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_VARIANCE, sizeof(PelorusVarianceSection), &p,
                                &got) == PEL_OK);
    {
        const PelorusVarianceSection *v = p;
        CHECK(v->texture_energy == 0.33f);
    }
}

static void check_roundtrip_filmgrain(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_OK);
    {
        const PelorusFilmGrainSection *g = p;
        CHECK(g->seed == 0xDEADBEEFCAFEULL);
        CHECK(g->num_y_points == 3);
        CHECK(g->apply == 1);
    }
}

static void check_roundtrip_sections(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    check_roundtrip_banding(blob, len);
    check_roundtrip_variance(blob, len);
    check_roundtrip_filmgrain(blob, len);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_ERR_ABSENT);
}

/* Round-trip: pack three sections, parse them back, verify scalars + framing. */
static void test_roundtrip(void)
{
    PelorusSideData meta;
    PelorusBandingSection band;
    PelorusVarianceSection var;
    PelorusFilmGrainSection grain;
    PelorusPackSection secs[3];
    uint8_t *blob = NULL;
    size_t len = 0;

    fill_meta(&meta);

    memset(&band, 0, sizeof(band));
    band.global_banding_risk = 0.42f;
    band.flat_area_fraction = 0.61f;
    band.contour_strength_mean = 0.03f;
    band.dominant_band_luma = 0.18f;

    memset(&var, 0, sizeof(var));
    var.global_variance = 0.25f;
    var.edge_density = 0.10f;
    var.texture_energy = 0.33f;

    memset(&grain, 0, sizeof(grain));
    grain.apply = 1;
    grain.seed = 0xDEADBEEFCAFEULL; /* exercises 8-byte alignment of u64 */
    grain.num_y_points = 3;
    grain.scaling_shift = 8;
    grain.ar_coeff_lag = 2;

    secs[0].id = PEL_SEC_BANDING;
    secs[0].data = &band;
    secs[0].size = (uint32_t)sizeof(band);
    secs[1].id = PEL_SEC_VARIANCE;
    secs[1].data = &var;
    secs[1].size = (uint32_t)sizeof(var);
    secs[2].id = PEL_SEC_FILMGRAIN;
    secs[2].data = &grain;
    secs[2].size = (uint32_t)sizeof(grain);

    CHECK(pel_blob_pack(&meta, secs, 3, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, len) == 1);
    check_roundtrip_sections(blob, len);

    pel_blob_free(blob);
}

/* Forward-compat (R4): a consumer that knows a SMALLER struct than the
 * producer wrote must get min(producer, consumer) readable bytes. */
static void test_forward_compat(void)
{
    PelorusSideData meta;
    PelorusVarianceSection var;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    const size_t older_consumer_size = 12; /* knew only the first 3 floats */

    fill_meta(&meta);
    memset(&var, 0, sizeof(var));
    var.global_variance = 1.0f;
    sec.id = PEL_SEC_VARIANCE;
    sec.data = &var;
    sec.size = (uint32_t)sizeof(var);

    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_VARIANCE, older_consumer_size, &p, &got) ==
          PEL_OK);
    CHECK(got == older_consumer_size); /* clamped to what the consumer knows */
    pel_blob_free(blob);
}

/* Header and directory patches go through memcpy on the byte buffer: the blob
 * is a uint8_t array, so no struct pointer is formed over it. */
static PelorusSideData blob_header_load(const uint8_t *blob)
{
    PelorusSideData hdr;

    memcpy(&hdr, blob + PELORUS_SIDEDATA_UUID_LEN, sizeof(hdr));
    return hdr;
}

static void blob_header_store(uint8_t *blob, const PelorusSideData *hdr)
{
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN, hdr, sizeof(*hdr));
}

/* R6: an ABI-major mismatch is detected and rejected, not misread. */
static void test_abi_major_mismatch(void)
{
    PelorusSideData meta;
    PelorusBandingSection band;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    PelorusSideData hdr;

    fill_meta(&meta);
    memset(&band, 0, sizeof(band));
    sec.id = PEL_SEC_BANDING;
    sec.data = &band;
    sec.size = (uint32_t)sizeof(band);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    hdr = blob_header_load(blob);
    hdr.abi_major = (uint16_t)(PELORUS_ABI_MAJOR + 1u);
    blob_header_store(blob, &hdr);

    CHECK(pel_blob_is_present(blob, len) == 0);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &p,
                                &got) == PEL_ERR_ABI);
    pel_blob_free(blob);
}

/* A non-Pelorus buffer (e.g. an x264 user-data SEI) is cleanly ignored. */
static void test_foreign_buffer(void)
{
    uint8_t foreign[64];
    const void *p = NULL;
    size_t got = 0;

    memset(foreign, 0xAB, sizeof(foreign));
    CHECK(pel_blob_is_present(foreign, sizeof(foreign)) == 0);
    CHECK(pel_blob_find_section(foreign, sizeof(foreign), PEL_SEC_BANDING,
                                sizeof(PelorusBandingSection), &p, &got) == PEL_ERR_ABSENT);
}

/* A header-only blob (no sections) is valid and parses. */
static void test_header_only(void)
{
    PelorusSideData meta;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_meta(&meta);
    CHECK(pel_blob_pack(&meta, NULL, 0, &blob, &len) == PEL_OK);
    CHECK(pel_blob_is_present(blob, len) == 1);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_BANDING, sizeof(PelorusBandingSection), &p,
                                &got) == PEL_ERR_ABSENT);
    pel_blob_free(blob);
}

/* Truncating the buffer is detected, not read out of bounds. */
static void test_truncation(void)
{
    PelorusSideData meta;
    PelorusMotionSection mv;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    fill_meta(&meta);
    memset(&mv, 0, sizeof(mv));
    sec.id = PEL_SEC_MOTION;
    sec.data = &mv;
    sec.size = (uint32_t)sizeof(mv);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    /* Lie about the length: claim only the uuid + header are present. */
    CHECK(pel_blob_find_section(blob, (size_t)PELORUS_SIDEDATA_UUID_LEN + sizeof(PelorusSideData),
                                PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_ERR_TRUNCATED);
    pel_blob_free(blob);
}

/* Defensive parser (R5): a crafted/corrupt blob whose section payload offset is
 * NOT 8-byte aligned must be rejected, not cast to a struct at an unaligned
 * address (the packer always 8-aligns, so this only arises from foreign/corrupt
 * framing). The current suite never patches dir[i].offset, so exercise it here. */
static void test_misaligned_offset(void)
{
    PelorusSideData meta;
    PelorusFilmGrainSection grain; /* has a u64 at offset 0 -> alignment matters */
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    PelorusSideData hdr;
    PelorusSectionDir dir;
    size_t dir_off;

    fill_meta(&meta);
    memset(&grain, 0, sizeof(grain));
    grain.seed = 0xDEADBEEFCAFEULL;
    sec.id = PEL_SEC_FILMGRAIN;
    sec.data = &grain;
    sec.size = (uint32_t)sizeof(grain);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    /* Well-formed first: the 8-aligned offset parses. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_OK);

    /* Now hand-patch dir[0].offset to a misaligned value (+4). The section then
     * still fits the buffer but its start is no longer 8-aligned. */
    hdr = blob_header_load(blob);
    dir_off = (size_t)PELORUS_SIDEDATA_UUID_LEN + hdr.header_size;
    memcpy(&dir, blob + dir_off, sizeof(dir));
    dir.offset += 4u;
    memcpy(blob + dir_off, &dir, sizeof(dir));

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_ERR_ABI);
    pel_blob_free(blob);
}

/* Defensive packer (R5 overflow guard): a section whose declared size, once
 * 8-aligned and summed, would overflow the uint32 total_size wire field must be
 * rejected before allocating (otherwise the alloc undersizes and the copy
 * overflows the heap). The size check runs before any deref of sec.data, so a
 * tiny dummy data pointer with a huge declared size is safe to pass here. */
static void test_pack_size_overflow(void)
{
    PelorusSideData meta;
    PelorusPackSection sec;
    uint8_t dummy = 0;
    uint8_t *blob = NULL;
    size_t len = 0;

    fill_meta(&meta);
    /* size near UINT32_MAX: 8-aligning it wraps a 32-bit accumulator to ~0 but the
     * 64-bit guard catches need > UINT32_MAX and rejects. data is never read. */
    sec.id = PEL_SEC_BANDING;
    sec.data = &dummy;
    sec.size = 0xFFFFFFF9u;
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_ERR_RANGE);
    CHECK(blob == NULL);
}

static void check_qp_report_scalars(const PelorusQpReportSection *r)
{
    CHECK(r->avg_qp == 27.5f);
    CHECK(r->psnr_y == 41.2f);
    CHECK(r->total_bits == 1234567ULL);
    CHECK(r->num_inter_blocks == 200);
    CHECK(r->honored_fraction == 0.75f);
    CHECK(r->report_source == PEL_QPSRC_QSV);
    CHECK(r->block_size_log2 == 4);
}

static void check_qp_report_fields(const PelorusQpReportSection *r, uint16_t cells)
{
    check_qp_report_scalars(r);
    CHECK(r->qp_valid == 1);
    CHECK(r->qp_cell_size == cells);
    CHECK(r->num_intra_blocks == 40);
    CHECK(r->num_skipped_blocks == 16);
    CHECK(r->psnr_u == 0.0f);
    CHECK(r->psnr_v == 0.0f);
}

static void check_qp_report_section(const uint8_t *blob, size_t len, uint16_t cells)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_QPREPORT, sizeof(PelorusQpReportSection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusQpReportSection));
    check_qp_report_fields(p, cells);

    /* An older consumer (knows only the first two floats) still parses (R4). */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_QPREPORT, 8, &p, &got) == PEL_OK);
    CHECK(got == 8);
}

/* PEL_SEC_QPREPORT (f): pack the encoder-honored QP readback with a per-cell
 * QP map appended after the blob, parse it back, verify scalars + the map. */
static void test_qp_report_roundtrip(void)
{
    PelorusSideData meta;
    PelorusQpReportSection qp;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const uint16_t cells = 16 * 9; /* matches fill_meta grid */
    int8_t cellmap[16 * 9];
    int i;

    fill_meta(&meta);

    memset(&qp, 0, sizeof(qp));
    qp.avg_qp = 27.5f;
    qp.psnr_y = 41.2f;
    qp.total_bits = 1234567ULL; /* exercises the 64-bit field alignment */
    qp.num_intra_blocks = 40;
    qp.num_inter_blocks = 200;
    qp.num_skipped_blocks = 16;
    qp.honored_fraction = 0.75f;
    qp.report_source = PEL_QPSRC_QSV;
    qp.block_size_log2 = 4; /* 16x16 */
    qp.qp_valid = 1;
    qp.qp_cell_size = cells; /* int8 per cell */
    /* qp_cell_offset is set below once we know the section's blob offset. */

    for (i = 0; i < (int)cells; i++) {
        cellmap[i] = (int8_t)(20 + (i % 12)); /* a recognizable QP ramp */
    }

    sec.id = PEL_SEC_QPREPORT;
    sec.data = &qp;
    sec.size = (uint32_t)sizeof(qp);

    /* Pack the section and verify the scalars + the map offset/size fields
     * round-trip (the per-cell map payload itself is appended by the producer
     * after pack, like every other map section — see docs/api/interop-abi.md;
     * the fold path is exercised separately in test_qp_report_fold). */
    qp.qp_cell_offset = 0; /* producer sets this when it appends cellmap */
    (void)cellmap;
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, len) == 1);
    check_qp_report_section(blob, len, cells);

    pel_blob_free(blob);
}

static void check_motion_conf_pair(const PelorusSideData *meta,
                                   const PelorusMotionConfSection *conf)
{
    PelorusMotionSection motion;
    PelorusPackSection sections[2];
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;

    memset(&motion, 0, sizeof(motion));
    sections[0].id = PEL_SEC_MOTION;
    sections[0].data = &motion;
    sections[0].size = (uint32_t)sizeof(motion);
    sections[1].id = PEL_SEC_MOTION_CONF;
    sections[1].data = conf;
    sections[1].size = (uint32_t)sizeof(*conf);
    CHECK(pel_blob_pack(meta, sections, 2, &blob, &len) == PEL_OK);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_OK);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION_CONF, sizeof(PelorusMotionConfSection),
                                &p, &got) == PEL_OK);
    CHECK(got == sizeof(PelorusMotionConfSection));
    pel_blob_free(blob);
}

static void check_motion_conf_fields(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION_CONF, sizeof(PelorusMotionConfSection),
                                &p, &got) == PEL_OK);
    CHECK(got == sizeof(PelorusMotionConfSection));
    {
        const PelorusMotionConfSection *r = p;
        CHECK(r->conf_field_size == 16 * 9);
        CHECK(r->conf_metric == PEL_MOTION_CONF_SAD);
    }

    /* R4: a consumer that knows only conf_field_offset+conf_field_size (8 bytes,
     * the meaning that predates conf_metric) still parses. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION_CONF, 8, &p, &got) == PEL_OK);
    CHECK(got == 8);

    /* R3: the plain motion section we did NOT write is absent. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_MOTION, sizeof(PelorusMotionSection), &p,
                                &got) == PEL_ERR_ABSENT);
}

/* PEL_SEC_MOTION_CONF (g): pack the per-block MV confidence section, parse it
 * back, verify the offset/size/metric fields round-trip; a consumer that knows
 * only the offset/size (not conf_metric) still parses (R4); and a section we did
 * not write is absent (R3). */
static void test_motion_conf_roundtrip(void)
{
    PelorusSideData meta;
    PelorusMotionConfSection conf;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;

    fill_meta(&meta);
    memset(&conf, 0, sizeof(conf));
    conf.conf_field_offset = 0;    /* producer patches once it appends the map */
    conf.conf_field_size = 16 * 9; /* matches fill_meta grid (uint8 per cell)  */
    conf.conf_metric = PEL_MOTION_CONF_SAD;

    sec.id = PEL_SEC_MOTION_CONF;
    sec.data = &conf;
    sec.size = (uint32_t)sizeof(conf);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);
    CHECK(pel_blob_is_present(blob, len) == 1);

    check_motion_conf_fields(blob, len);

    pel_blob_free(blob);
    /* Production writes MOTION and MOTION_CONF together; both must survive. */
    check_motion_conf_pair(&meta, &conf);
}

static void check_complexity_values(const PelorusComplexitySection *r)
{
    CHECK(r->complexity == 0.625f);
    CHECK(r->texture_energy == 0.5f);
    CHECK(r->motion_component == 0.25f);
    CHECK(r->has_scene_cut == 1);
}

static void check_complexity_fields(const uint8_t *blob, size_t len)
{
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_find_section(blob, len, PEL_SEC_COMPLEXITY, sizeof(PelorusComplexitySection), &p,
                                &got) == PEL_OK);
    CHECK(got == sizeof(PelorusComplexitySection));
    check_complexity_values(p);
    /* R4: a consumer that knows only `complexity` (first 4 bytes) still parses. */
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_COMPLEXITY, 4, &p, &got) == PEL_OK);
    CHECK(got == 4);
}

/* PEL_SEC_COMPLEXITY (h): pack the per-frame complexity scalar, round-trip the
 * fields, and confirm R4 (an older consumer that knows only the first float
 * still parses). */
static void test_complexity_roundtrip(void)
{
    PelorusSideData meta;
    PelorusComplexitySection cx;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;

    fill_meta(&meta);
    memset(&cx, 0, sizeof(cx));
    cx.complexity = 0.625f;
    cx.texture_energy = 0.5f;
    cx.motion_component = 0.25f;
    cx.has_scene_cut = 1;

    sec.id = PEL_SEC_COMPLEXITY;
    sec.data = &cx;
    sec.size = (uint32_t)sizeof(cx);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);
    CHECK(blob != NULL);

    check_complexity_fields(blob, len);
    pel_blob_free(blob);
}

static void check_fold_uniform(const PelorusQpReportSection *out, const int8_t *cells, size_t n)
{
    size_t i;

    CHECK(out->qp_valid == 1);
    CHECK(out->qp_cell_size == 4u * 2u);
    CHECK(out->report_source == PEL_QPSRC_QSV);
    CHECK(out->avg_qp == 30.0f);
    for (i = 0; i < n; i++) {
        CHECK(cells[i] == 30); /* uniform block QP -> uniform cell QP */
    }
}

static void check_fold_stats_only(const PelorusQpReportInput *in)
{
    PelorusQpReportSection out;

    CHECK(pel_qp_report_from_blocks(in, 4, 2, &out, NULL, 0) == PEL_OK);
    CHECK(out.qp_valid == 0);
    CHECK(out.qp_cell_size == 0u);
    CHECK(out.avg_qp == 30.0f);
}

/* The reader stub: fold a per-block QP grid onto the cell grid. With a block
 * grid that is an integer multiple of the cell grid, each cell averages a clean
 * block tile, so a uniform block QP folds to the same per-cell QP. */
static void test_qp_report_fold(void)
{
    PelorusQpReportInput in;
    PelorusQpReportSection out;
    int8_t blocks[8 * 4];
    int8_t cells[4 * 2];
    int i;

    memset(&in, 0, sizeof(in));
    for (i = 0; i < (int)(sizeof(blocks)); i++) {
        blocks[i] = 30; /* uniform actual QP across all blocks */
    }
    in.block_qp = blocks;
    in.blk_cols = 8;
    in.blk_rows = 4;
    in.block_size_log2 = 4;
    in.report_source = PEL_QPSRC_QSV;
    in.avg_qp = 30.0f;
    in.num_inter_blocks = 32;

    CHECK(pel_qp_report_from_blocks(&in, 4, 2, &out, cells, sizeof(cells)) == PEL_OK);
    check_fold_uniform(&out, cells, sizeof(cells));

    /* Frame-stats-only path: no block grid -> qp_valid 0, scalars preserved. */
    in.block_qp = NULL;
    check_fold_stats_only(&in);

    /* Too-small output buffer is rejected, not overrun. */
    in.block_qp = blocks;
    CHECK(pel_qp_report_from_blocks(&in, 4, 2, &out, cells, 3) == PEL_ERR_RANGE);

    /* NULL inputs / zero grid are rejected. */
    CHECK(pel_qp_report_from_blocks(NULL, 4, 2, &out, cells, sizeof(cells)) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_blocks(&in, 0, 2, &out, cells, sizeof(cells)) == PEL_ERR_INVALID);
}

static void check_x265_fold_scalars(const PelorusQpReportSection *qp)
{
    CHECK(qp->qp_valid == 0);
    CHECK(qp->report_source == PEL_QPSRC_NONE);
    CHECK(qp->total_bits == 35000ULL);
    /* bit-weighted: (26*24000 + 30*8000 + 34*3000)/35000 = 27.6 exactly. */
    CHECK(qp->avg_qp > 27.55f && qp->avg_qp < 27.65f);
    CHECK(qp->psnr_y > 41.0f && qp->psnr_y < 43.2f); /* I-frame-dominated */
    CHECK(qp->honored_fraction == 0.0f);             /* no request to compare */
}

static void check_x265_fold(const PelorusX265Frame *frames, size_t count)
{
    PelorusQpReportSection qp;
    const float requested_flat[3] = {28.0f, 28.0f, 28.0f};
    const float requested_shaped[3] = {24.0f, 30.0f, 36.0f};

    CHECK(pel_qp_report_from_x265_frames(frames, count, NULL, &qp) == PEL_OK);
    check_x265_fold_scalars(&qp);

    /* Flat requested QP agrees only with the achieved mean frame: 1/3. */
    CHECK(pel_qp_report_from_x265_frames(frames, count, requested_flat, &qp) == PEL_OK);
    CHECK(qp.honored_fraction > 0.33f && qp.honored_fraction < 0.34f);

    /* A requested shape matching achieved low/flat/high QP agrees everywhere. */
    CHECK(pel_qp_report_from_x265_frames(frames, count, requested_shaped, &qp) == PEL_OK);
    CHECK(qp.honored_fraction == 1.0f);
}

static void check_x265_frames(const PelorusX265Frame *frames)
{
    CHECK(frames[0].slice_type == 'I');
    CHECK(frames[0].qp == 26.0f);
    CHECK(frames[0].bits == 24000ULL);
    CHECK(frames[1].slice_type == 'P');
    CHECK(frames[2].qp == 34.0f);
    CHECK(frames[2].psnr_v == 35.5f);
}

/* The x265 CSV reader (ADR-0122): write a minimal x265-shaped CSV to a temp
 * file, parse it, fold it into a PEL_SEC_QPREPORT, and verify the aggregated
 * scalars + the requested-vs-honored honored_fraction. This is the runnable
 * closed-loop surface; the fixture exercises it without invoking x265. */
static void test_x265_csv_reader(void)
{
    /* Three coded frames + a trailing aggregate row x265 appends. Columns match
     * x265 --csv-log-level 2 (subset; the reader locates by header name). The
     * honored QP differs from a flat requested QP per slice type, exactly the
     * signal honored_fraction measures. */
    static const char *csv = "Encode Order, Type, POC, QP, Bits, Y PSNR, U PSNR, V PSNR\n"
                             "0, I-SLICE, 0, 26.00, 24000, 43.1, 40.5, 40.4\n"
                             "1, P-SLICE, 2, 30.00, 8000, 38.0, 37.2, 36.8\n"
                             "2, B-SLICE, 1, 34.00, 3000, 37.1, 36.6, 35.5\n"
                             "Total frames, 3, , 30.00, , , , \n";
    PelorusX265Frame frames[8];
    size_t count = 0;
    const char *path = "pelorus_x265_csv_test.csv";
    int fixture_rc;

    fixture_rc = create_checked_fixture(path, csv);
    CHECK(fixture_rc == 0);
    if (fixture_rc != 0) {
        return;
    }
    /* Once created, every later step falls through to the remove() below. */

    /* Parse: 3 coded frames, the "Total frames" aggregate row dropped. */
    CHECK(pel_x265_csv_parse(path, frames, 8, &count) == PEL_OK);
    CHECK(count == 3);
    check_x265_frames(frames);

    check_x265_fold(frames, count);

    /* A capacity smaller than the row count truncates and reports RANGE. */
    CHECK(pel_x265_csv_parse(path, frames, 2, &count) == PEL_ERR_RANGE);
    CHECK(count == 2);

    CHECK(remove(path) == 0);
}

/* The x265 CSV reader's error paths need no fixture file. */
static void test_x265_csv_reader_guards(void)
{
    PelorusX265Frame frames[8];
    size_t count = 0;
    PelorusQpReportSection qp;

    memset(frames, 0, sizeof(frames));
    /* A missing file is ABSENT, not a crash. */
    CHECK(pel_x265_csv_parse("pelorus_no_such_file.csv", frames, 8, &count) == PEL_ERR_ABSENT);

    /* NULL guards. */
    CHECK(pel_x265_csv_parse(NULL, frames, 8, &count) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_x265_frames(NULL, 1, NULL, &qp) == PEL_ERR_INVALID);
    CHECK(pel_qp_report_from_x265_frames(frames, 0, NULL, &qp) == PEL_ERR_INVALID);
}

/* The deband param contract: defaults validate, out-of-range is rejected. */
static void test_deband_params(void)
{
    PelorusDebandParams pp;
    const char *what = NULL;

    pel_deband_params_default(&pp);
    CHECK(pel_deband_params_validate(&pp, &what) == PEL_OK);

    pp.range = 99;
    CHECK(pel_deband_params_validate(&pp, &what) == PEL_ERR_RANGE);
    CHECK(what != NULL && strcmp(what, "range") == 0);
}

/* Parse one re-homed copy of the film-grain blob and read the payload through
 * memcpy, as a careful consumer does. */
static void check_skewed_blob(const uint8_t *skewed, size_t len)
{
    PelorusFilmGrainSection got_grain;
    const void *p = NULL;
    size_t got = 0;

    CHECK(pel_blob_is_present(skewed, len) == 1);
    CHECK(pel_blob_find_section(skewed, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_OK);
    CHECK(p != NULL && got == sizeof(PelorusFilmGrainSection));
    if (p != NULL && got == sizeof(got_grain)) {
        memcpy(&got_grain, p, sizeof(got_grain));
        CHECK(got_grain.seed == 0xDEADBEEFCAFEULL);
    }
}

/* Issue #44: the parser must not assume the caller's blob base is 8-byte
 * aligned. Before the memcpy-based parse this produced 26 -fsanitize=alignment
 * diagnostics and is genuine UB on strict-alignment targets. The section
 * pointer handed back is still only castable when the BASE was aligned, so the
 * check here reads the payload through memcpy, exactly as a careful consumer
 * (and vmafx's perceptual_weight.c) does. */
static void test_misaligned_blob_base(void)
{
    PelorusSideData meta;
    PelorusFilmGrainSection grain; /* u64 at offset 0 -> alignment matters */
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    uint8_t *raw = NULL;
    size_t len = 0;
    size_t skew;

    fill_meta(&meta);
    memset(&grain, 0, sizeof(grain));
    grain.seed = 0xDEADBEEFCAFEULL;
    sec.id = PEL_SEC_FILMGRAIN;
    sec.data = &grain;
    sec.size = (uint32_t)sizeof(grain);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    /* Re-home the blob at every misalignment in one 8-byte period. */
    raw = (uint8_t *)malloc(len + 8u);
    CHECK(raw != NULL);
    if (raw == NULL) {
        pel_blob_free(blob);
        return;
    }
    for (skew = 1u; skew < 8u; skew++) {
        memcpy(raw + skew, blob, len);
        check_skewed_blob(raw + skew, len);
    }

    free(raw);
    pel_blob_free(blob);
}

/* A header_size that is not a multiple of 8 would put dir[] on a misaligned
 * start. That is corrupt framing from an untrusted producer, not a short
 * buffer, so it must be rejected rather than walked. */
static void test_unaligned_header_size(void)
{
    PelorusSideData meta;
    PelorusFilmGrainSection grain;
    PelorusPackSection sec;
    uint8_t *blob = NULL;
    size_t len = 0;
    const void *p = NULL;
    size_t got = 0;
    PelorusSideData hdr;

    fill_meta(&meta);
    memset(&grain, 0, sizeof(grain));
    sec.id = PEL_SEC_FILMGRAIN;
    sec.data = &grain;
    sec.size = (uint32_t)sizeof(grain);
    CHECK(pel_blob_pack(&meta, &sec, 1, &blob, &len) == PEL_OK);

    hdr = blob_header_load(blob);
    hdr.header_size = (uint16_t)(hdr.header_size + 4u);
    blob_header_store(blob, &hdr);
    CHECK(pel_blob_find_section(blob, len, PEL_SEC_FILMGRAIN, sizeof(PelorusFilmGrainSection), &p,
                                &got) == PEL_ERR_ABI);

    pel_blob_free(blob);
}

int main(void)
{
    test_roundtrip();
    test_forward_compat();
    test_abi_major_mismatch();
    test_foreign_buffer();
    test_header_only();
    test_truncation();
    test_misaligned_offset();
    test_misaligned_blob_base();
    test_unaligned_header_size();
    test_pack_size_overflow();
    test_qp_report_roundtrip();
    test_motion_conf_roundtrip();
    test_complexity_roundtrip();
    test_qp_report_fold();
    test_fixture_file_safety();
    test_x265_csv_reader();
    test_x265_csv_reader_guards();
    test_deband_params();

    if (g_fail != 0) {
        (void)fprintf(stderr, "%d check(s) failed\n", g_fail);
        return EXIT_FAILURE;
    }
    (void)printf("interop: all checks passed (libpelorus %s, ABI %u.%u)\n",
                 pelorus_version_string(), PELORUS_ABI_MAJOR, PELORUS_ABI_MINOR);
    return EXIT_SUCCESS;
}

/* NOLINTEND(modernize-use-nullptr) */
