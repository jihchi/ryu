// Copyright 2018 Ulf Adams
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ryu/ryu.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FLOAT_MANTISSA_BITS 23
#define FLOAT_EXPONENT_BITS 8

#define LOG10_2_DENOMINATOR 10000000L
#define LOG10_2_NUMERATOR 3010299L // LOG10_2_DENOMINATOR * log_10(2)
#define LOG10_5_DENOMINATOR 10000000L
#define LOG10_5_NUMERATOR 6989700L // LOG10_5_DENOMINATOR * log_10(5)
#define LOG2_5_DENOMINATOR 10000000L
#define LOG2_5_NUMERATOR 23219280L // LOG2_5_DENOMINATOR * log_2(5)

#define POW5_INV_BITCOUNT 59
static uint64_t POW5_INV_SPLIT[31] = {
  576460752303423489u,  461168601842738791u,  368934881474191033u,  295147905179352826u,
  472236648286964522u,  377789318629571618u,  302231454903657294u,  483570327845851670u,
  386856262276681336u,  309485009821345069u,  495176015714152110u,  396140812571321688u,
  316912650057057351u,  507060240091291761u,  405648192073033409u,  324518553658426727u,
  519229685853482763u,  415383748682786211u,  332306998946228969u,  531691198313966350u,
  425352958651173080u,  340282366920938464u,  544451787073501542u,  435561429658801234u,
  348449143727040987u,  557518629963265579u,  446014903970612463u,  356811923176489971u,
  570899077082383953u,  456719261665907162u,  365375409332725730u
};
#define POW5_BITCOUNT 61
static uint64_t POW5_SPLIT[47] = {
 1152921504606846976u, 1441151880758558720u, 1801439850948198400u, 2251799813685248000u,
 1407374883553280000u, 1759218604441600000u, 2199023255552000000u, 1374389534720000000u,
 1717986918400000000u, 2147483648000000000u, 1342177280000000000u, 1677721600000000000u,
 2097152000000000000u, 1310720000000000000u, 1638400000000000000u, 2048000000000000000u,
 1280000000000000000u, 1600000000000000000u, 2000000000000000000u, 1250000000000000000u,
 1562500000000000000u, 1953125000000000000u, 1220703125000000000u, 1525878906250000000u,
 1907348632812500000u, 1192092895507812500u, 1490116119384765625u, 1862645149230957031u,
 1164153218269348144u, 1455191522836685180u, 1818989403545856475u, 2273736754432320594u,
 1421085471520200371u, 1776356839400250464u, 2220446049250313080u, 1387778780781445675u,
 1734723475976807094u, 2168404344971008868u, 1355252715606880542u, 1694065894508600678u,
 2117582368135750847u, 1323488980084844279u, 1654361225106055349u, 2067951531382569187u,
 1292469707114105741u, 1615587133892632177u, 2019483917365790221u
};

#ifndef NO_DIGIT_TABLE
static const char DIGIT_TABLE[200] = {
  '0','0','0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8','0','9',
  '1','0','1','1','1','2','1','3','1','4','1','5','1','6','1','7','1','8','1','9',
  '2','0','2','1','2','2','2','3','2','4','2','5','2','6','2','7','2','8','2','9',
  '3','0','3','1','3','2','3','3','3','4','3','5','3','6','3','7','3','8','3','9',
  '4','0','4','1','4','2','4','3','4','4','4','5','4','6','4','7','4','8','4','9',
  '5','0','5','1','5','2','5','3','5','4','5','5','5','6','5','7','5','8','5','9',
  '6','0','6','1','6','2','6','3','6','4','6','5','6','6','6','7','6','8','6','9',
  '7','0','7','1','7','2','7','3','7','4','7','5','7','6','7','7','7','8','7','9',
  '8','0','8','1','8','2','8','3','8','4','8','5','8','6','8','7','8','8','8','9',
  '9','0','9','1','9','2','9','3','9','4','9','5','9','6','9','7','9','8','9','9'
};
#endif // NO_DIGIT_TABLE

static inline int32_t min(int32_t a, int32_t b) {
  return a < b ? a : b;
}

static uint32_t pow5Factor(uint32_t value) {
  for (uint32_t count = 0; value > 0; count++) {
    if (value - 5 * (value / 5) != 0) {
      return count;
    }
    value /= 5;
  }
  return 0;
}

static inline uint32_t pow5bits(int32_t e) {
  return e == 0 ? 1 : (uint32_t) ((e * LOG2_5_NUMERATOR + LOG2_5_DENOMINATOR - 1) / LOG2_5_DENOMINATOR);
}

// It seems to be slightly faster to avoid uint128_t here, although the
// generated code for uint128_t looks slightly nicer.
static inline uint32_t mulPow5InvDivPow2(uint32_t m, uint32_t q, int32_t j) {
  uint64_t factor = POW5_INV_SPLIT[q];
  uint64_t bits0 = ((uint64_t) m) * (factor & 0xffffffff);
  uint64_t bits1 = ((uint64_t) m) * (factor >> 32);
  return (uint32_t) (((bits0 >> 32) + bits1) >> (j - 32));
}

static inline uint32_t mulPow5divPow2(uint32_t m, uint32_t i, int32_t j) {
  uint64_t factor = POW5_SPLIT[i];
  uint64_t bits0 = ((uint64_t) m) * (factor & 0xffffffff);
  uint64_t bits1 = ((uint64_t) m) * (factor >> 32);
  return (uint32_t) (((bits0 >> 32) + bits1) >> (j - 32));
}

static inline uint32_t decimalLength(uint32_t v) {
  if (v >= 1000000000) return 10;
  if (v >= 100000000) return 9;
  if (v >= 10000000) return 8;
  if (v >= 1000000) return 7;
  if (v >= 100000) return 6;
  if (v >= 10000) return 5;
  if (v >= 1000) return 4;
  if (v >= 100) return 3;
  if (v >= 10) return 2;
  return 1;
}

void f2s_buffered(float f, char* result) {
  uint32_t mantissaBits = FLOAT_MANTISSA_BITS;
  uint32_t exponentBits = FLOAT_EXPONENT_BITS;
  uint32_t offset = (1 << (exponentBits - 1)) - 1;

  uint32_t bits = 0;
  // This only works on little-endian architectures.
  memcpy(&bits, &f, sizeof(float));

  // Decode bits into sign, mantissa, and exponent.
  bool sign = ((bits >> (mantissaBits + exponentBits)) & 1) != 0;
  uint32_t ieeeMantissa = bits & ((1L << mantissaBits) - 1);
  uint32_t ieeeExponent = (uint32_t) ((bits >> mantissaBits) & ((1 << exponentBits) - 1));

  int32_t e2;
  uint32_t m2;
  // Case distinction; exit early for the easy cases.
  if (ieeeExponent == ((1u << exponentBits) - 1u)) {
    strcpy(result, (ieeeMantissa != 0) ? "NaN" : sign ? "-Infinity" : "Infinity");
    return;
  } else if (ieeeExponent == 0) {
    if (ieeeMantissa == 0) {
      strcpy(result, sign ? "-0E0" : "0E0");
      return;
    }
    e2 = 1 - offset - mantissaBits - 2;
    m2 = ieeeMantissa;
  } else {
    e2 = ieeeExponent - offset - mantissaBits - 2;
    m2 = (1 << mantissaBits) | ieeeMantissa;
  }
  bool even = (m2 & 1) == 0;
  bool acceptBounds = even;

  // Compute the upper and lower bounds.
#ifdef NICER_OUTPUT
  uint32_t mv = 4 * m2;
#endif
  uint32_t mp = 4 * m2 + 2;
  uint32_t mm = 4 * m2 - (((m2 != (1L << mantissaBits)) || (ieeeExponent <= 1)) ? 2 : 1);

#ifdef NICER_OUTPUT
  uint32_t vr;
#endif
  uint32_t vp, vm;
  int32_t e10;
  bool vmIsTrailingZeros = false;
  if (e2 >= 0) {
    int32_t q = (int32_t) ((e2 * LOG10_2_NUMERATOR) / LOG10_2_DENOMINATOR);
    e10 = q;
    int32_t k = POW5_INV_BITCOUNT + pow5bits(q) - 1;
    int32_t i = -e2 + q + k;
#ifdef NICER_OUTPUT
    vr = mulPow5InvDivPow2(mv, q, i);
#endif
    vp = mulPow5InvDivPow2(mp, q, i);
    vm = mulPow5InvDivPow2(mm, q, i);
    if (mp % 5 == 0) {
      if (acceptBounds) {
        vmIsTrailingZeros = 0 >= q;
      } else {
        vp -= min(e2 + 1, pow5Factor(mp)) >= q;
      }
    } else {
      if (acceptBounds) {
        vmIsTrailingZeros = min(e2 + (~mm & 1), pow5Factor(mm)) >= q;
      } else {
        vp -= 0 >= q;
      }
    }
  } else {
    int32_t q = (int32_t) ((-e2 * LOG10_5_NUMERATOR) / LOG10_5_DENOMINATOR);
    e10 = q + e2;
    int32_t i = -e2 - q;
    int32_t k = pow5bits(i) - POW5_BITCOUNT;
    int32_t j = q - k;
#ifdef NICER_OUTPUT
    vr = mulPow5divPow2(mv, i, j);
#endif
    vp = mulPow5divPow2(mp, i, j);
    vm = mulPow5divPow2(mm, i, j);
    if (acceptBounds) {
      vmIsTrailingZeros = (~mm & 1) >= q;
    } else {
      vp -= 1 >= q;
    }
  }

  uint32_t vplength = decimalLength(vp);
  int32_t exp = e10 + vplength - 1;

//  printf("%i %i\n", vpIsTrailingZeros, acceptBounds);

  uint32_t removed = 0;
  while (vp / 10 > vm / 10) {
    vmIsTrailingZeros &= vm % 10 == 0;
#ifdef NICER_OUTPUT
    vr /= 10;
#endif
    vp /= 10;
    vm /= 10;
    removed++;
  }
  if (vmIsTrailingZeros && acceptBounds) {
    while (vm % 10 == 0) {
#ifdef NICER_OUTPUT
      vr /= 10;
#endif
      vp /= 10;
      vm /= 10;
      removed++;
    }
  }
#ifdef NICER_OUTPUT
  uint32_t output = vr > vm ? vr : vp;
#else
  uint32_t output = vp;
#endif
  uint32_t olength = vplength - removed;
//  printf("%i %i %i\n", output, vplength, olength);

  int index = 0;
  if (sign) {
    result[index++] = '-';
  }

#ifndef NO_DIGIT_TABLE
  // Print decimal digits after the decimal point.
  int32_t i = 0;
  while (output >= 10000) {
    uint32_t c = output - 10000 * (output / 10000); // output % 10000;
    output /= 10000;
    uint32_t c0 = (c % 100) << 1;
    uint32_t c1 = (c / 100) << 1;
    memcpy(result + index + olength - i - 1, DIGIT_TABLE + c0, 2);
    memcpy(result + index + olength - i - 3, DIGIT_TABLE + c1, 2);
    i += 4;
  }
  while (output >= 100) {
    uint32_t c = (output - 100 * (output / 100)) << 1; // (output % 100) << 1;
    output /= 100;
    memcpy(result + index + olength - i - 1, DIGIT_TABLE + c, 2);
    i += 2;
  }
  if (output >= 10) {
    uint32_t c = output << 1;
    result[index + olength - i] = DIGIT_TABLE[c + 1];
    result[index] = DIGIT_TABLE[c];
  } else {
    // Print the leading decimal digit.
    result[index] = '0' + output;
  }
#else
  // Print decimal digits after the decimal point.
  for (uint32_t i = 0; i < olength - 1; i++) {
    uint32_t c = output % 10; output /= 10;
    result[index + olength - i] = '0' + c;
  }
  // Print the leading decimal digit.
  result[index] = '0' + output % 10;
#endif // NO_DIGIT_TABLE

  // Print decimal point if needed.
  if (olength > 1) {
    result[index + 1] = '.';
    index += olength + 1;
  } else {
    index++;
  }

  // Print the exponent.
  result[index++] = 'E';
  if (exp < 0) {
    result[index++] = '-';
    exp = -exp;
  }

#ifndef NO_DIGIT_TABLE
  if (exp >= 10) {
    memcpy(result + index, DIGIT_TABLE + (2 * exp), 2);
    index += 2;
  } else {
    result[index++] = '0' + exp;
  }
#else
  if (exp >= 10) {
    result[index++] = '0' + (exp / 10) % 10;
  }
  result[index++] = '0' + exp % 10;
#endif // NO_DIGIT_TABLE

  // Terminate the string.
  result[index++] = '\0';
}

char* f2s(float f) {
  char* result = (char*) calloc(16, sizeof(char));
  f2s_buffered(f, result);
  return result;
}
