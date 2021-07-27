#ifndef THREEWISE_XOR_BINARY_FUSE_FILTER_XOR_FILTER_SORTED_H_
#define THREEWISE_XOR_BINARY_FUSE_FILTER_XOR_FILTER_SORTED_H_
#include "xor_binary_fuse_filter.h"
namespace xorbinaryfusefilter_sorted {
// status returned by a xor filter operation
enum Status {
  Ok = 0,
  NotFound = 1,
  NotEnoughSpace = 2,
  NotSupported = 3,
};

inline uint64_t rotl64(uint64_t n, unsigned int c) {
  // assumes width is a power of 2
  const unsigned int mask = (CHAR_BIT * sizeof(n) - 1);
  // assert ( (c<=mask) &&"rotate by type width or more");
  c &= mask;
  return (n << c) | (n >> ((-c) & mask));
}

__attribute__((always_inline)) inline uint32_t reduce(uint32_t hash,
                                                      uint32_t n) {
  // http://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
  return (uint32_t)(((uint64_t)hash * n) >> 32);
}

template <typename ItemType, typename FingerprintType,
          typename HashFamily = SimpleMixSplit>
class XorBinaryFuseFilter {
public:
  size_t size;
  size_t arrayLength;
  size_t segmentCount;
  size_t segmentCountLength;
  size_t segmentLength;
  size_t segmentLengthMask;
  static constexpr size_t arity = 3;
  FingerprintType *fingerprints;

  HashFamily *hasher;

  inline FingerprintType fingerprint(const uint64_t hash) const {
    return (FingerprintType)hash;
  }

  inline __attribute__((always_inline)) size_t getHashFromHash(uint64_t hash,
                                                               int index) {
    __uint128_t x = (__uint128_t)hash * (__uint128_t)segmentCountLength;
    uint64_t h = (uint64_t)(x >> 64);
    h += index * segmentLength;
    // keep the lower 36 bits
    uint64_t hh = hash & ((1UL << 36) - 1);
    // index 0: right shift by 36; index 1: right shift by 18; index 2: no shift
    h ^= (size_t)((hh >> (36 - 18 * index)) & segmentLengthMask);
    return h;
  }

  explicit XorBinaryFuseFilter(const size_t size) {
    hasher = new HashFamily();
    this->size = size;
    this->segmentLength = calculateSegmentLength(arity, size);
    // the current implementation hardcodes a 18-bit limit to
    // to the segment length.
    if (this->segmentLength > (1 << 18)) {
      this->segmentLength = (1 << 18);
    }
    double sizeFactor = calculateSizeFactor(arity, size);
    size_t capacity = size * sizeFactor;
    size_t segmentCount =
        (capacity + segmentLength - 1) / segmentLength - (arity - 1);
    this->arrayLength = (segmentCount + arity - 1) * segmentLength;
    this->segmentLengthMask = this->segmentLength - 1;
    this->segmentCount =
        (this->arrayLength + this->segmentLength - 1) / this->segmentLength;
    this->segmentCount =
        this->segmentCount <= arity - 1 ? 1 : this->segmentCount - (arity - 1);
    this->arrayLength = (this->segmentCount + arity - 1) * this->segmentLength;
    this->segmentCountLength = this->segmentCount * this->segmentLength;
    fingerprints = new FingerprintType[arrayLength]();
    std::fill_n(fingerprints, arrayLength, 0);
  }

  ~XorBinaryFuseFilter() {
    delete[] fingerprints;
    delete hasher;
  }

  Status AddAll(const vector<ItemType> &data, const size_t start,
                const size_t end) {
    return AddAll(data.data(), start, end);
  }

  Status AddAll(const ItemType *data, const size_t start, const size_t end);

  // Report if the item is inserted, with false positive rate.
  Status Contain(const ItemType &item) const;

  /* methods for providing stats  */
  // summary infomation
  std::string Info() const;

  // number of current inserted items;
  size_t Size() const { return size; }

  // size of the filter in bytes.
  size_t SizeInBytes() const { return arrayLength * sizeof(FingerprintType); }
};

struct t2val {
  uint64_t t2;
  uint64_t t2count;
};

typedef struct t2val t2val_t;

template <typename ItemType, typename FingerprintType, typename HashFamily>
Status XorBinaryFuseFilter<ItemType, FingerprintType, HashFamily>::AddAll(
    const ItemType *keys, const size_t start, const size_t end) {

  uint64_t *hashes = new uint64_t[size];
  uint64_t *reverseOrder = new uint64_t[size];
  uint8_t *reverseH = new uint8_t[size];
  size_t reverseOrderPos;

  t2val_t *t2vals = new t2val_t[arrayLength];

  size_t *alone = new size_t[arrayLength];
  size_t hashIndex{0};

  while (true) {
    memset(t2vals, 0, sizeof(t2val_t[arrayLength]));

    // counting sort - this is just a first implementation
    // Let us adjust with respect to segmentCountLength
    int blockBits = 1;
    while((size_t(1)<<blockBits) < segmentCount) { blockBits++; }
    size_t *startPos = new size_t[1 << blockBits]();
    for (size_t i = start; i < end; i++) {
      uint64_t k = keys[i];
      uint64_t hash = (*hasher)(k);
      // we could re-use the keys array, because we can reverse the hashing
      hashes[i] = hash;
      startPos[hash >> (64 - blockBits)]++;
    }
    for(int i = 1; i < (1<<blockBits); i++) {
      startPos[i] += startPos[i - 1];
    }
    for (size_t i = start; i < end; i++) {
      uint64_t hash = hashes[i];
      // hack: re-use reverseOrder array
      reverseOrder[--startPos[hash >> (64 - blockBits)]] = hash;
    }
    for (size_t i = 0; i < size; i++) {
      uint64_t hash = reverseOrder[i];
      for (int hi = 0; hi < 3; hi++) {
        int index = getHashFromHash(hash, hi);
        t2vals[index].t2count++;
        t2vals[index].t2 ^= hash;
      }
    }
    delete[] startPos;

    reverseOrderPos = 0;
    size_t alonePos = 0;
    for (size_t i = 0; i < arrayLength; i++) {
      if (t2vals[i].t2count == 1) {
        alone[alonePos++] = i;
      }
    }

    while (alonePos > 0) {
      alonePos--;
      size_t index = alone[alonePos];
      if (t2vals[index].t2count == 1) {
        // It is still there!
        uint64_t hash = t2vals[index].t2;
        reverseOrder[reverseOrderPos] = hash;
        for (int hi = 0; hi < 3; hi++) {
          size_t index3 = getHashFromHash(hash, hi);
          if (index3 == index) {
            reverseH[reverseOrderPos] = hi;
            // no need to decrement & remove
            continue;
          } else if (t2vals[index3].t2count == 2) {
            // Found a new candidate !
            alone[alonePos++] = index3;
          }
          t2vals[index3].t2count -= 1;
          t2vals[index3].t2 ^= hash;
        }
        reverseOrderPos++;
      }
    }

    if (reverseOrderPos == size) {
      break;
    }
    hashIndex++;
    // use a new random numbers
    delete hasher;
    hasher = new HashFamily();
  }
  delete[] alone;
  delete[] t2vals;
  delete[] hashes;

  for (int i = reverseOrderPos - 1; i >= 0; i--) {
    // the hash of the key we insert next
    uint64_t hash = reverseOrder[i];
    int found = reverseH[i];
    // which entry in the table we can change
    int change = -1;
    // we set table[change] to the fingerprint of the key,
    // unless the other two entries are already occupied
    FingerprintType xor2 = fingerprint(hash);

    for (int hi = 0; hi < 3; hi++) {
      size_t h = getHashFromHash(hash, hi);
      if (found == hi) {
        change = h;
      } else {
        // this is different from BDZ: using xor to calculate the
        // fingerprint
        xor2 ^= fingerprints[h];
      }
    }
    fingerprints[change] = xor2;
  }
  delete[] reverseOrder;
  delete[] reverseH;

  return Ok;
}

template <typename ItemType, typename FingerprintType, typename HashFamily>
Status XorBinaryFuseFilter<ItemType, FingerprintType, HashFamily>::Contain(
    const ItemType &key) const {
  uint64_t hash = (*hasher)(key);
  FingerprintType f = fingerprint(hash);
  __uint128_t x = (__uint128_t)hash * (__uint128_t)segmentCountLength;
  int h0 = (uint64_t)(x >> 64);
  int h1 = h0 + segmentLength;
  int h2 = h1 + segmentLength;
  uint64_t hh = hash;
  h1 ^= (size_t)((hh >> 18) & segmentLengthMask);
  h2 ^= (size_t)((hh)&segmentLengthMask);
  f ^= fingerprints[h0] ^ fingerprints[h1] ^ fingerprints[h2];
  return f == 0 ? Ok : NotFound;
}

template <typename ItemType, typename FingerprintType, typename HashFamily>
std::string
XorBinaryFuseFilter<ItemType, FingerprintType, HashFamily>::Info() const {
  std::stringstream ss;
  ss << "XorBinaryFuseFilter Status:\n"
     << "\t\tKeys stored: " << Size() << "\n";
  return ss.str();
}
} // namespace xorbinaryfusefilter_sorted
#endif