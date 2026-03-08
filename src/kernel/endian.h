/*
 * endian.h — Endian-aware accessors for on-disk data
 *
 * Provides le16()/le32() for reading/writing little-endian data
 * (UFS, FAT32/VFAT) and be16()/be32() for big-endian data.
 *
 * On ARM (little-endian):  le16/le32 are identity, be16/be32 swap.
 * On m68k (big-endian):    le16/le32 swap, be16/be32 are identity.
 *
 * romfs and ELF use target-native endianness and do NOT need these
 * accessors — they are built per-target by mkromfs / cross-compiler.
 *
 * GCC defines __BYTE_ORDER__ on both ARM and m68k cross-compilers.
 */

#ifndef PPAP_ENDIAN_H
#define PPAP_ENDIAN_H

#include <stdint.h>

/* ── Little-endian accessors ─────────────────────────────────────────────
 *
 * Convert between little-endian on-disk format and native CPU order.
 * Used by: ufs.c, vfat.c (FAT32 is always little-endian per spec).
 * ────────────────────────────────────────────────────────────────────────── */

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

static inline uint16_t le16(uint16_t v) { return v; }
static inline uint32_t le32(uint32_t v) { return v; }

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

static inline uint16_t le16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t le32(uint32_t v) { return __builtin_bswap32(v); }

#else
#error "Unknown byte order — define le16/le32 for this platform"
#endif

/* ── Big-endian accessors ────────────────────────────────────────────────
 *
 * Convert between big-endian on-disk format and native CPU order.
 * Currently unused; provided for completeness (XFS-style formats, etc.).
 * ────────────────────────────────────────────────────────────────────────── */

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

static inline uint16_t be16(uint16_t v) { return v; }
static inline uint32_t be32(uint32_t v) { return v; }

#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

#else
#error "Unknown byte order — define be16/be32 for this platform"
#endif

#endif /* PPAP_ENDIAN_H */
