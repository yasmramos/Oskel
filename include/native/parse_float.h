#pragma once

#include <cstddef>
#include <cstdint>

#include "native/codegen.h"
#include "native/dtoa.h"

// Freestanding decimal/hex text -> binary64 parser for the AOT runtime
// (PHASE 7c.5b.2b). It backs `float(string)` and `double(string)`.
//
// The hosted engine parses the text with `strtod`, which is *correctly
// rounded*: the result is the binary64 value nearest to the exact decimal
// value, with ties to even. Reproducing it needs the exact value, so this is a
// small fixed bignum (32-bit limbs in 64-bit slots, the same representation
// `dtoa.cpp` uses) rather than a floating-point approximation -- a `double`
// cannot hold the exact value of most decimal strings.
//
// The algorithm was prototyped and validated in C++ against `strtod` over
// hundreds of thousands of random inputs (binary64, subnormals and hex floats)
// before being emitted, so the emitted code only has to match a known-correct
// reference. It needs no big/big division: a negative exponent divides by 10
// repeatedly and a positive one multiplies by 5, so the whole thing runs on the
// `bn_*` primitives `dtoa.cpp` already emits.
namespace parse_float {

// Bigger than dtoa's `kLimbs`: a mantissa of `kMaxDigits` decimal digits is
// ~2.6k bits, and the negative-exponent path scales it by `2^K` before the
// repeated division. 160 limbs (5120 bits) leaves comfortable headroom over
// the ~3.9k bits the worst case reaches.
constexpr int kParseLimbs = 160;

// A binary64 needs at most 767 significant decimal digits to be pinned down
// (the smallest subnormal), so keeping this many loses no information.
constexpr int kMaxDigits = 768;

// Hex floats keep 20 digits (80 bits): more than the 53 a binary64 holds, so
// the rounding decision is exact once the rest becomes a sticky bit.
constexpr int kMaxHexDigits = 20;

struct ParseBignum {
  std::int64_t n;
  std::uint64_t w[kParseLimbs];
};

// Scratch block for one parse. Declared as a struct so the emitter uses
// `offsetof` for every field instead of hand-counted byte offsets, which is
// how a layout bug would otherwise slip in silently.
struct ParseScratch {
  ParseBignum d;            // the working integer (decimal or hex mantissa)
  char digits[kMaxDigits];  // significant digits, as raw 0..15 values
  std::int64_t out;         // the caller's output word
  std::int64_t neg;         // sign
  std::int64_t total;       // significant digits stored in `digits`
  std::int64_t frac;        // digits after the radix point
  std::int64_t extra;       // significant digits dropped (cap + trailing zeros)
  std::int64_t sticky;      // 1 when a discarded digit was nonzero
  std::int64_t exp10;       // explicit decimal exponent (e/E)
  std::int64_t pexp;        // explicit binary exponent (p/P, hex only)
  std::int64_t net;         // net decimal exponent (decimal path)
  std::int64_t sExp;        // binary exponent of `d`
  std::int64_t bits;        // the assembled bit pattern
  std::int64_t seenDot;     // a radix point was consumed
  std::int64_t seenDigit;   // at least one digit was consumed
  std::int64_t started;     // a nonzero significant digit was seen
  std::int64_t anyHex;      // at least one hex digit was consumed
  std::int64_t fracHex;     // hex digits after the radix point
  std::int64_t tmp;         // generic temporary
  std::int64_t i;           // loop counter
  std::int64_t cnt;         // loop counter
  std::int64_t L;           // bit length of `d`
  std::int64_t E;           // floor(log2(value))
  std::int64_t ulp;         // exponent of the significand's lowest bit
  std::int64_t shift;       // sExp - ulp
  std::int64_t rb;          // the round bit
  std::int64_t st;          // the sticky bit
  std::int64_t t2;          // the rounded significand
};

constexpr std::size_t kParseScratchBytes = sizeof(ParseScratch);

struct ParseFloatHandles {
  // long oskel_parse_double(const char* data, long length, long* outBits,
  //                         ParseScratch* scratch)
  // Returns 0 on success (the bit pattern is written to `*outBits`). On failure
  // the code names the cause, because the hosted engine reports a different
  // message for each: 1 is malformed text, 2 is a magnitude outside the
  // binary64 range (the hosted `(use inf, -inf or nan)` case) and 3 is text
  // that is empty after trimming.
  CodeGen::Label parseDouble;
};

inline ParseFloatHandles newParseFloatHandles(CodeGen& code) {
  ParseFloatHandles handles;
  handles.parseDouble = code.newLabel();
  return handles;
}

// Emits the parser. It calls the `bn_*` helpers in `dtoa`, so `dtoa` must be
// emitted into the same code buffer. It preserves rbx, rbp and r12-r15, like
// every other runtime routine.
void emitParseFloat(CodeGen& code, const ParseFloatHandles& handles,
                    const dtoa::DtoaHandles& dtoa);

}  // namespace parse_float
