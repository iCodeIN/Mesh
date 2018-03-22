// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/**
 * @file   bitmap.h
 * @brief  A bitmap class, with one bit per element.
 * @author Emery Berger <http://www.cs.umass.edu/~emery>
 * @note   Copyright (C) 2005 by Emery Berger, University of Massachusetts Amherst.
 */

#pragma once
#ifndef MESH__BITMAP_H
#define MESH__BITMAP_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "internal.h"

#include "static/staticlog.h"

#include "heaplayers.h"

namespace mesh {
namespace internal {

using std::atomic_size_t;

// enables iteration through the set bits of the bitmap
template <typename Container>
class BitmapIter : public std::iterator<std::forward_iterator_tag, size_t> {
public:
  BitmapIter(const Container &a, const size_t i) : _i(i), _cont(a) {
  }
  BitmapIter &operator++() {
    if (unlikely(_i + 1 >= _cont.bitCount())) {
      _i = _cont.bitCount();
      return *this;
    }

    _i = _cont.lowestSetBitAt(_i + 1);
    return *this;
  }
  bool operator==(const BitmapIter &rhs) const {
    return _cont.bits() == rhs._cont.bits() && _i == rhs._i;
  }
  bool operator!=(const BitmapIter &rhs) const {
    return _cont.bits() != rhs._cont.bits() || _i != rhs._i;
  }
  size_t &operator*() {
    return _i;
  }

private:
  size_t _i;
  const Container &_cont;
};


class AtomicBitmapBase {}


/// To find the bit in a word, do this: word & getMask(bitPosition)
/// @return a "mask" for the given position.
static inline size_t getMask(uint64_t pos) {
  return 1UL << pos;
}

static inline bool atomicSet(atomic_size_t *bits, uint32_t item, uint32_t position) {
  const auto mask = getMask(position);

  size_t oldValue = bits[item];
  while (!std::atomic_compare_exchange_weak(&bits[item],  // address of word
                                            &oldValue,    // old val
                                            oldValue | mask)) {
  }

  return !(oldValue & mask);
}

static inline bool atomicUnset(atomic_size_t *bits, uint32_t item, uint32_t position) {
  const auto mask = getMask(position);

  size_t oldValue = bits[item];
  while (!std::atomic_compare_exchange_weak(&bits[item],  // address of word
                                            &oldValue,    // old val
                                            oldValue & ~mask)) {
  }

  return !(oldValue & mask);
}

static inline bool relaxedSet(size_t *bits, uint32_t item, uint32_t position) {
  const auto mask = getMask(position);

  size_t oldValue = bits[item];
  bits[item] = oldValue | mask;

  return !(oldValue & mask);
}

/// Clears the bit at the given index.
static inline bool relaxedUnset(size_t *bits, uint32_t item, uint32_t position) {
  const auto mask = getMask(position);

  size_t oldValue = bits[item];
  bits[item] = oldValue & ~mask;

  return !(oldValue & mask);
}

template <
  typename word_t,
  bool (*setAt)(word_t *bits, uint32_t item, uint32_t position),
  bool (*unsetAt)(word_t *bits, uint32_t item, uint32_t position)>
class BitmapBase {
private:
  DISALLOW_COPY_AND_ASSIGN(BitmapBase);

  typedef BitmapBase<word_t, setAt, unsetAt> Bitmap;

  static_assert(sizeof(size_t) == sizeof(atomic_size_t), "no overhead atomics");
  static_assert(sizeof(word_t) == sizeof(size_t), "word_t should be size_t");

  /// A synonym for the datatype corresponding to a word.
  enum { WORD_BITS = sizeof(word_t) * 8 };
  enum { WORD_BYTES = sizeof(word_t) };
  /// The log of the number of bits in a size_t, for shifting.
  enum { WORD_BITSHIFT = staticlog(WORD_BITS) };

  BitmapBase() = delete;

public:
  typedef BitmapIter<Bitmap> iterator;
  typedef BitmapIter<Bitmap> const const_iterator;

  explicit BitmapBase(size_t bitCount)
      : _bitCount(bitCount), _bitarray(reinterpret_cast<word_t *>(internal::Heap().malloc(byteCount()))) {
    d_assert(_bitarray != nullptr);
    clear();
  }

  explicit BitmapBase(const std::string &str)
      : _bitCount(str.length()), _bitarray(reinterpret_cast<word_t *>(internal::Heap().malloc(byteCount()))) {
    d_assert(_bitarray != nullptr);
    clear();

    for (size_t i = 0; i < str.length(); ++i) {
      char c = str[i];
      d_assert_msg(c == '0' || c == '1', "expected 0 or 1 in bitstring, not %c ('%s')", c, str.c_str());
      if (c == '1')
        tryToSet(i);
    }
  }

  explicit BitmapBase(const internal::string &str)
      : _bitCount(str.length()), _bitarray(reinterpret_cast<word_t *>(internal::Heap().malloc(byteCount()))) {
    d_assert(_bitarray != nullptr);
    clear();

    for (size_t i = 0; i < str.length(); ++i) {
      char c = str[i];
      d_assert_msg(c == '0' || c == '1', "expected 0 or 1 in bitstring, not %c ('%s')", c, str.c_str());
      if (c == '1')
        tryToSet(i);
    }
  }

  BitmapBase(BitmapBase &&rhs) : _bitCount(rhs._bitCount), _bitarray(rhs._bitarray) {
    rhs._bitarray = nullptr;
  }

  ~BitmapBase() {
    if (_bitarray)
      internal::Heap().free(_bitarray);
    _bitarray = nullptr;
  }

  internal::string to_string(ssize_t bitCount = -1) const {
    if (bitCount == -1)
      bitCount = _bitCount;
    d_assert(0 <= bitCount && static_cast<size_t>(bitCount) <= _bitCount);

    internal::string s(bitCount, '0');

    for (ssize_t i = 0; i < bitCount; i++) {
      if (isSet(i))
        s[i] = '1';
    }

    return s;
  }

  // number of bytes used to store the bitmap -- rounds up to nearest sizeof(size_t)
  inline size_t byteCount() const {
    return WORD_BITS * ((_bitCount + WORD_BITS - 1) / WORD_BITS) / 8;
  }

  inline size_t bitCount() const {
    return _bitCount;
  }

  const word_t *bits() const {
    return _bitarray;
  }

  /// Clears out the bitmap array.
  void clear(void) {
    if (_bitarray != nullptr) {
      const auto wordCount = byteCount() / sizeof(size_t);
      // use an explicit array since these are atomic_size_t's
      for (size_t i = 0; i < wordCount; i++) {
        _bitarray[i] = 0;
      }
    }
  }

  inline uint64_t setFirstEmpty(uint64_t startingAt = 0) {
    uint32_t startWord, off;
    computeItemPosition(startingAt, startWord, off);

    const size_t words = byteCount();
    for (size_t i = startWord; i < words; i++) {
      const size_t bits = _bitarray[i];
      if (bits == ~0UL) {
        off = 0;
        continue;
      }

      d_assert(off <= 63U);
      size_t unsetBits = ~bits;
      d_assert(unsetBits != 0);

      // if the offset is 3, we want to mark the first 3 bits as 'set'
      // or 'unavailable'.
      unsetBits &= ~((1UL << off) - 1);

      // if, after we've masked off everything below our offset there
      // are no free bits, continue
      if (unsetBits == 0) {
        off = 0;
        continue;
      }

      // debug("unset bits: %zx (off: %u, startingAt: %llu", unsetBits, off, startingAt);

      size_t off = __builtin_ffsll(unsetBits) - 1;
      const bool ok = setAt(_bitarray, i, off);
      // if we couldn't set the bit, we raced with a different thread.  try again.
      if (!ok) {
        off++;
        continue;
      }

      return WORD_BITS * i + off;
    }

    debug("mesh: bitmap completely full, aborting.\n");
    abort();
  }

  /// @return true iff the bit was not set (but it is now).
  inline bool tryToSet(uint64_t index) {
    uint32_t item, position;
    computeItemPosition(index, item, position);
    return setAt(_bitarray, item, position);
  }

  /// Clears the bit at the given index.
  inline bool unset(uint64_t index) {
    uint32_t item, position;
    computeItemPosition(index, item, position);

    return unsetAt(_bitarray, item, position);
  }

  // FIXME: who uses this? bad idea with atomics
  inline bool isSet(uint64_t index) const {
    uint32_t item, position;
    computeItemPosition(index, item, position);

    return _bitarray[item] & getMask(position);
  }

  inline uint64_t inUseCount() const {
    const auto wordCount = byteCount() / 8;
    uint64_t count = 0;
    for (size_t i = 0; i < wordCount; i++) {
      count += __builtin_popcountl(_bitarray[i]);
    }
    return count;
  }

  iterator begin() {
    return iterator(*this, lowestSetBitAt(0));
  }
  iterator end() {
    return iterator(*this, bitCount());
  }
  const_iterator begin() const {
    return iterator(*this, lowestSetBitAt(0));
  }
  const_iterator end() const {
    return iterator(*this, bitCount());
  }
  const_iterator cbegin() const {
    return iterator(*this, lowestSetBitAt(0));
  }
  const_iterator cend() const {
    return iterator(*this, bitCount());
  }

  size_t lowestSetBitAt(uint64_t startingAt) const {
    uint32_t startWord, startOff;
    computeItemPosition(startingAt, startWord, startOff);

    for (size_t i = startWord; i < byteCount(); i++) {
      const auto mask = ~((1UL << startOff) - 1);
      const auto bits = _bitarray[i] & mask;
      startOff = 0;

      if (bits == 0ULL)
        continue;

      const size_t off = __builtin_ffsl(bits) - 1;

      const auto bit = WORD_BITS * i + off;
      return bit < bitCount() ? bit : bitCount();
    }

    return bitCount();
  }

private:
  /// Given an index, compute its item (word) and position within the word.
  inline void computeItemPosition(uint64_t index, uint32_t &item, uint32_t &position) const {
    d_assert(index < _bitCount);
    item = index >> WORD_BITSHIFT;
    position = index & (WORD_BITS - 1);
    d_assert(position == index - (item << WORD_BITSHIFT));
    d_assert(item < byteCount() / 8);
  }

  const size_t _bitCount{0};
  word_t *_bitarray{nullptr};
};

// typedef BitmapBase<atomic_size_t, mesh::internal::atomicSet, mesh::internal::atomicUnset> Bitmap;
typedef BitmapBase<size_t, mesh::internal::relaxedSet, mesh::internal::relaxedUnset> Bitmap;

static_assert(sizeof(Bitmap) == sizeof(size_t) * 2, "Bitmap unexpected size");

}  // namespace internal
}  // namespace mesh

#endif  // MESH__BITMAP_H
