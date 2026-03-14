#ifndef __STORFS_COMMON_H__
#define __STORFS_COMMON_H__

#include <stdint.h>

#define ARRAY_SIZE(x)        (sizeof(x) / sizeof(*(x)))
#define DIV_BY_8(val)        (val >> 3)
#define MULT_BY_8(val)       (val << 3)
#define CEIL_DIV(num, denom) ((num + denom - 1) / denom)

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/*
 * cross platform, cross compiler, cross architecture breakpoint
 * https://gist.github.com/prashantrahul141/ff121cd1747d6175f2e83478388815bb
 */
#if !defined(BREAKPOINT)
#if defined(_MSC_VER)
#define BREAKPOINT() __debugbreak()
#elif defined(__clang__)
#define BREAKPOINT() __builtin_debugtrap()
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define BREAKPOINT() __asm__ volatile("int3;nop")
#elif defined(__GNUC__) && defined(__thumb__)
#define BREAKPOINT() __asm__ volatile(".inst 0xde01")
#elif defined(__GNUC__) && defined(__arm__) && !defined(__thumb__)
#define BREAKPOINT() __asm__ volatile(".inst 0xe7f001f0")
#else
#if defined(__cplusplus)
#include <cassert>
#else
#include <assert.h>
#endif
#define BREAKPOINT() assert(0)
#endif
#endif  // BREAKPOINT

#if !defined(UNUSED)
#define UNUSED(x) ((void)(x))
#endif  // UNUSED

/* Helper conversion function */
uint16_t uint8_t_to_uint16_t(uint8_t *buf, uint32_t *index);
uint32_t uint8_t_to_uint32_t(uint8_t *buf, uint32_t *index);
uint64_t uint8_t_to_uint64_t(uint8_t *buf, uint32_t *index);
void     uint16_t_to_uint8_t(uint8_t *buf, uint16_t uint16Val, uint32_t *index);
void     uint32_t_to_uint8_t(uint8_t *buf, uint32_t uint32Val, uint32_t *index);
void     uint64_t_to_uint8_t(uint8_t *buf, uint64_t uint64Val, uint32_t *index);

#endif  // __STORFS_COMMON_H__
