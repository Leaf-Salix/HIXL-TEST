#pragma once
#include <cstddef>
#include <cstdint>

// Mix window identity and absolute byte position; linear byte patterns can alias across ranks.
inline uint8_t Pattern(int test, int rank, int kind, size_t offset) {
  uint64_t value = (static_cast<uint64_t>(test) << 32) ^
                   (static_cast<uint64_t>(rank) << 16) ^ (static_cast<uint64_t>(kind) << 8);
  value ^= static_cast<uint64_t>(offset) * 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return static_cast<uint8_t>(value ^ (value >> 31));
}
