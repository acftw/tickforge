// Two-level bitmap over price levels.
//
// Bit i is set when price level i has resting orders. Finding the next
// non-empty level above or below a price is the hot path when the best level
// empties: instead of walking a tree or scanning levels one by one, we scan
// 64 levels per machine word and skip empty words using a summary bitmap.
// For a 65,536-level book that is at most a handful of word operations.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tf {

class LevelBitmap {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  explicit LevelBitmap(std::size_t n)
      : n_(n), words_((n + 63) / 64, 0), summary_((words_.size() + 63) / 64, 0) {}

  std::size_t size() const { return n_; }

  bool test(std::size_t i) const { return (words_[i >> 6] >> (i & 63)) & 1ULL; }

  void set(std::size_t i) {
    const std::size_t w = i >> 6;
    words_[w] |= bit(i);
    summary_[w >> 6] |= bit(w);
  }

  void clear(std::size_t i) {
    const std::size_t w = i >> 6;
    words_[w] &= ~bit(i);
    if (words_[w] == 0) summary_[w >> 6] &= ~bit(w);
  }

  // Lowest set index >= i, or npos.
  std::size_t next(std::size_t i) const {
    if (i >= n_) return npos;
    std::size_t w = i >> 6;
    const std::uint64_t m = words_[w] & (~0ULL << (i & 63));
    if (m) return (w << 6) + static_cast<std::size_t>(std::countr_zero(m));
    const std::size_t wn = w + 1;
    if (wn >= words_.size()) return npos;
    std::size_t s = wn >> 6;
    std::uint64_t sm = summary_[s] & (~0ULL << (wn & 63));
    while (sm == 0) {
      if (++s >= summary_.size()) return npos;
      sm = summary_[s];
    }
    const std::size_t w2 = (s << 6) + static_cast<std::size_t>(std::countr_zero(sm));
    return (w2 << 6) + static_cast<std::size_t>(std::countr_zero(words_[w2]));
  }

  // Highest set index <= i, or npos.
  std::size_t prev(std::size_t i) const {
    if (n_ == 0) return npos;
    if (i >= n_) i = n_ - 1;
    const std::size_t w = i >> 6;
    const std::uint64_t m = words_[w] & (~0ULL >> (63 - (i & 63)));
    if (m) return (w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(m));
    if (w == 0) return npos;
    const std::size_t wp = w - 1;
    std::size_t s = wp >> 6;
    std::uint64_t sm = summary_[s] & (~0ULL >> (63 - (wp & 63)));
    while (sm == 0) {
      if (s == 0) return npos;
      sm = summary_[--s];
    }
    const std::size_t w2 = (s << 6) + 63 - static_cast<std::size_t>(std::countl_zero(sm));
    return (w2 << 6) + 63 - static_cast<std::size_t>(std::countl_zero(words_[w2]));
  }

 private:
  static constexpr std::uint64_t bit(std::size_t i) { return 1ULL << (i & 63); }

  std::size_t n_;
  std::vector<std::uint64_t> words_;
  std::vector<std::uint64_t> summary_;
};

}  // namespace tf
