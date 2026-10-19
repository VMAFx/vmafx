/**
 *  Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * De-tiling of Intel-tiled planes on the device (ADR-1121, ADR-2091): one
 * definition of the address math for the VA-surface import
 * (dmabuf_import.cpp) and the VMAFx dma-buf import (vmafx_sycl_rt.cpp).
 *
 * Both layouts tile a plane in 4 KiB tiles of 128 bytes x 32 rows, placed
 * row-major across the plane's pitch (a multiple of 128). Inside a tile:
 * - Y tiles (I915_FORMAT_MOD_Y_TILED) store 16-byte columns ("OWords") of 32
 *   rows one after another;
 * - Tile4 (I915_FORMAT_MOD_4_TILED, Xe-HPG) swizzles the byte address
 *   bits as x[3:0] y[1:0] x[5:4] y[2] x[6] y[3] y[4].
 * A work-item moves one 4-byte word; the last word of a row may hold fewer
 * bytes of the row than four. A 16-bit sample is shifted right by `shift`
 * as it is stored (P010 / P012 MSB alignment, fused into the store); 0
 * stores the bytes unchanged. Integer arithmetic only, no private arrays
 * (scratch-free, ADR-1395).
 */

#ifndef VMAF_SRC_SYCL_DETILE_H_
#define VMAF_SRC_SYCL_DETILE_H_

#include <sycl/sycl.hpp>

#include <cstddef>
#include <cstdint>

namespace vmaf_sycl_detile
{

/* Bytes of a tile row, rows of a tile, bytes of a tile. */
inline constexpr unsigned kTileWidth = 128u;
inline constexpr unsigned kTileRows = 32u;
inline constexpr size_t kTileBytes = 4096u;
/* 4-byte words of a tile row. */
inline constexpr unsigned kTileWords = kTileWidth / 4u;

/* What one de-tile launch reads and writes. */
struct Plane {
    const uint8_t *src; /* first tile of the plane */
    uint8_t *dst;       /* first linear row */
    size_t dst_pitch;   /* bytes between linear rows (>= row_bytes) */
    size_t row_bytes;   /* bytes of a row the plane holds */
    unsigned tiles_per_row;
    unsigned rows;
    unsigned shift; /* right shift of each 16-bit sample, or 0 */
    bool tile4;     /* Tile4 (modifier 9); false: Y-tiled (modifier 2) */
};

/* Byte offset inside the tile of the word at tile column byte `x_byte`
 * (a multiple of 4) on tile row `ity`. */
inline size_t in_tile_offset(bool tile4, unsigned x_byte, unsigned ity)
{
    if (!tile4) {
        return (size_t)(x_byte / 16u) * 512u + (size_t)ity * 16u + (x_byte % 16u);
    }
    return (size_t)((x_byte & 0x0Fu) | ((ity & 3u) << 4) | (((x_byte >> 4) & 3u) << 6) |
                    (((ity >> 2) & 1u) << 8) | (((x_byte >> 6) & 1u) << 9) |
                    (((ity >> 3) & 1u) << 10) | (((ity >> 4) & 1u) << 11));
}

/* Both 16-bit samples of a little-endian word, shifted right by `shift`. */
inline uint32_t shift_pair(uint32_t v, unsigned shift)
{
    const auto lo = (uint32_t)((uint16_t)(v & 0xFFFFu) >> shift);
    const auto hi = (uint32_t)((uint16_t)(v >> 16) >> shift);
    return lo | (hi << 16);
}

/* The little-endian word at `in`: one 32-bit load when it is aligned. */
inline uint32_t load_word(const uint8_t *in)
{
    if (((uintptr_t)in & 3u) == 0u) {
        return *(const uint32_t *)in;
    }
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

/* Store `v` little-endian at `out`: one 32-bit store when it is aligned. */
inline void store_full(uint8_t *out, uint32_t v)
{
    if (((uintptr_t)out & 3u) == 0u) {
        *(uint32_t *)out = v;
        return;
    }
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)(v >> 24);
}

/* Store the word of row `py`, word column `word_x`. */
inline void store_word(const Plane &p, unsigned py, unsigned word_x)
{
    const unsigned tc = word_x / kTileWords;
    const unsigned x_byte = (word_x % kTileWords) * 4u;
    const size_t col = (size_t)tc * kTileWidth + x_byte;
    if (col >= p.row_bytes) {
        return;
    }
    const size_t src_off = ((size_t)(py / kTileRows) * p.tiles_per_row + tc) * kTileBytes +
                           in_tile_offset(p.tile4, x_byte, py % kTileRows);
    uint8_t *const out = p.dst + (size_t)py * p.dst_pitch + col;
    const uint8_t *const in = p.src + src_off;
    if (col + 4u <= p.row_bytes) {
        const uint32_t v = load_word(in);
        store_full(out, p.shift ? shift_pair(v, p.shift) : v);
        return;
    }
    /* The row ends inside this word: 1 to 3 bytes (a 16-bit plane: 2). */
    const size_t remain = p.row_bytes - col;
    const uint32_t lo = (uint32_t)in[0] | ((uint32_t)in[1] << 8);
    const uint32_t v = p.shift && remain == 2u ? shift_pair(lo, p.shift) : lo;
    out[0] = (uint8_t)(v & 0xFFu);
    if (remain >= 2u) {
        out[1] = (uint8_t)((v >> 8) & 0xFFu);
    }
    if (remain >= 3u) {
        out[2] = in[2];
    }
}

/* Enqueue the de-tile of one plane on `q`: rows x the words of the tiled
 * pitch, one word per work-item. */
inline sycl::event launch(sycl::queue &q, const Plane &p)
{
    const size_t words = (size_t)p.tiles_per_row * kTileWords;
    return q.parallel_for(sycl::range<2>(p.rows, words),
                          [=](sycl::id<2> id) { store_word(p, (unsigned)id[0], (unsigned)id[1]); });
}

} // namespace vmaf_sycl_detile

#endif /* VMAF_SRC_SYCL_DETILE_H_ */
