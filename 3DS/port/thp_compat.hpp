// Stand-in for aurora's lib/internal.hpp when building its THP decoder
// (extern/aurora/lib/dolphin/thp) for the 3DS: only ByteReader is needed, and
// the real header pulls in fmt and the rest of aurora.
#pragma once

// port/movie_3ds.c provides THPVideoDecode (build-time ETC1 frames) and falls
// back to this decoder when the movie was not converted; port/movie_audio_3ds.c
// does the same for THPAudioDecode
#define THPVideoDecode THPVideoDecode_sw
#define THPAudioDecode THPAudioDecode_sw

#include "dolphin/thp.h"

// Software MJPEG decoding costs ~110 ms per frame on the 3DS CPU, so movie
// frames are output black (the player, its timing and audio run as usual)
#define THP_3DS_DECODE_VIDEO 0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

// Big-endian integer load
template <typename T>
inline T read_bits(const void* ptr) noexcept {
  const uint8_t* p = static_cast<const uint8_t*>(ptr);
  uint64_t v = 0;
  for (size_t i = 0; i < sizeof(T); i++) {
    v = (v << 8) | p[i];
  }
  return static_cast<T>(v);
}

namespace aurora {

// Reads big-endian values from a byte buffer
class ByteReader {
public:
  ByteReader(const uint8_t* data, size_t size) noexcept : mData{data}, mSize{size} {}

  static ByteReader unbounded(const void* data) noexcept {
    return {static_cast<const uint8_t*>(data), std::numeric_limits<size_t>::max()};
  }

  size_t offset() const noexcept { return mPosition; }
  size_t remaining() const noexcept { return mSize - mPosition; }

  template <typename T>
  T read() noexcept {
    T value{};
    if (can_read(sizeof(T))) {
      value = load<T>(mData + mPosition);
    }
    mPosition += sizeof(T);
    return value;
  }

  template <typename T>
  bool try_read(T& value) noexcept {
    if (!can_read(sizeof(T))) {
      return false;
    }
    value = load<T>(mData + mPosition);
    mPosition += sizeof(T);
    return true;
  }

  bool try_take(size_t count, std::span<const uint8_t>& bytes) noexcept {
    if (!can_read(count)) {
      return false;
    }
    bytes = {mData + mPosition, count};
    mPosition += count;
    return true;
  }

  void skip(size_t count) noexcept { mPosition += count; }

private:
  template <typename T>
  static T load(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); i++) {
      v = (v << 8) | p[i];
    }
    if constexpr (sizeof(T) == 4 && std::numeric_limits<T>::is_iec559) {
      uint32_t u = static_cast<uint32_t>(v);
      T f;
      std::memcpy(&f, &u, 4);
      return f;
    } else {
      return static_cast<T>(v);
    }
  }

  bool can_read(size_t count) const noexcept { return mPosition <= mSize && count <= mSize - mPosition; }

  const uint8_t* mData;
  size_t mSize;
  size_t mPosition = 0;
};

} // namespace aurora
