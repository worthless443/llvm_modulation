//===-- Single-precision cos function -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/math/cosf.h"
#include "sincosf_utils.h"
#include "src/__support/FPUtil/BasicOperations.h"
#include "src/__support/FPUtil/FEnvImpl.h"
#include "src/__support/FPUtil/FPBits.h"
#include "src/__support/FPUtil/except_value_utils.h"
#include "src/__support/FPUtil/multiply_add.h"
#include "src/__support/common.h"

#include <errno.h>

namespace __llvm_libc {

// Exceptional cases for cosf.
static constexpr int COSF_EXCEPTS = 6;

static constexpr fputil::ExceptionalValues<float, COSF_EXCEPTS> CosfExcepts{
    /* inputs */ {
        0x55325019, // x = 0x1.64a032p43
        0x5922aa80, // x = 0x1.4555p51
        0x5aa4542c, // x = 0x1.48a858p54
        0x5f18b878, // x = 0x1.3170fp63
        0x6115cb11, // x = 0x1.2b9622p67
        0x7beef5ef, // x = 0x1.ddebdep120
    },
    /* outputs (RZ, RU offset, RD offset, RN offset) */
    {
        {0x3f4ea5d2, 1, 0, 0}, // x = 0x1.64a032p43, cos(x) = 0x1.9d4ba4p-1 (RZ)
        {0x3f08aebe, 1, 0, 1}, // x = 0x1.4555p51, cos(x) = 0x1.115d7cp-1 (RZ)
        {0x3efa40a4, 1, 0, 0}, // x = 0x1.48a858p54, cos(x) = 0x1.f48148p-2 (RZ)
        {0x3f7f14bb, 1, 0, 0}, // x = 0x1.3170fp63, cos(x) = 0x1.fe2976p-1 (RZ)
        {0x3f78142e, 1, 0, 1}, // x = 0x1.2b9622p67, cos(x) = 0x1.f0285cp-1 (RZ)
        {0x3f08a21c, 1, 0,
         0}, // x = 0x1.ddebdep120, cos(x) = 0x1.114438p-1 (RZ)
    }};

LLVM_LIBC_FUNCTION(float, cosf, (float x)) {
  using FPBits = typename fputil::FPBits<float>;
  FPBits xbits(x);
  xbits.set_sign(false);

  uint32_t x_abs = xbits.uintval();
  double xd = static_cast<double>(xbits.get_val());

  // Range reduction:
  // For |x| > pi/16, we perform range reduction as follows:
  // Find k and y such that:
  //   x = (k + y) * pi/16
  //   k is an integer
  //   |y| < 0.5
  // For small range (|x| < 2^46 when FMA instructions are available, 2^22
  // otherwise), this is done by performing:
  //   k = round(x * 16/pi)
  //   y = x * 16/pi - k
  // For large range, we will omit all the higher parts of 16/pi such that the
  // least significant bits of their full products with x are larger than 31,
  // since cos((k + y + 32*i) * pi/16) = cos(x + i * 2pi) = cos(x).
  //
  // When FMA instructions are not available, we store the digits of 16/pi in
  // chunks of 28-bit precision.  This will make sure that the products:
  //   x * SIXTEEN_OVER_PI_28[i] are all exact.
  // When FMA instructions are available, we simply store the digits of 16/pi in
  // chunks of doubles (53-bit of precision).
  // So when multiplying by the largest values of single precision, the
  // resulting output should be correct up to 2^(-208 + 128) ~ 2^-80.  By the
  // worst-case analysis of range reduction, |y| >= 2^-38, so this should give
  // us more than 40 bits of accuracy. For the worst-case estimation of range
  // reduction, see for instances:
  //   Elementary Functions by J-M. Muller, Chapter 11,
  //   Handbook of Floating-Point Arithmetic by J-M. Muller et. al.,
  //   Chapter 10.2.
  //
  // Once k and y are computed, we then deduce the answer by the cosine of sum
  // formula:
  //   cos(x) = cos((k + y)*pi/16)
  //          = cos(y*pi/16) * cos(k*pi/16) - sin(y*pi/16) * sin(k*pi/16)
  // The values of sin(k*pi/16) and cos(k*pi/16) for k = 0..31 are precomputed
  // and stored using a vector of 32 doubles. Sin(y*pi/16) and cos(y*pi/16) are
  // computed using degree-7 and degree-8 minimax polynomials generated by
  // Sollya respectively.

  // |x| < 0x1.0p-12f
  if (unlikely(x_abs < 0x3980'0000U)) {
    // When |x| < 2^-12, the relative error of the approximation cos(x) ~ 1
    // is:
    //   |cos(x) - 1| < |x^2 / 2| = 2^-25 < epsilon(1)/2.
    // So the correctly rounded values of cos(x) are:
    //   = 1 - eps(x) if rounding mode = FE_TOWARDZERO or FE_DOWWARD,
    //   = 1 otherwise.
    // To simplify the rounding decision and make it more efficient and to
    // prevent compiler to perform constant folding, we use
    //   fma(x, -2^-25, 1) instead.
    // Note: to use the formula 1 - 2^-25*x to decide the correct rounding, we
    // do need fma(x, -2^-25, 1) to prevent underflow caused by -2^-25*x when
    // |x| < 2^-125. For targets without FMA instructions, we simply use
    // double for intermediate results as it is more efficient than using an
    // emulated version of FMA.
#if defined(LIBC_TARGET_HAS_FMA)
    return fputil::multiply_add(xbits.get_val(), -0x1.0p-25f, 1.0f);
#else
    return static_cast<float>(fputil::multiply_add(xd, -0x1.0p-25, 1.0));
#endif // LIBC_TARGET_HAS_FMA
  }

  using ExceptChecker = typename fputil::ExceptionChecker<float, COSF_EXCEPTS>;
  {
    float result;
    if (ExceptChecker::check_odd_func(CosfExcepts, x_abs, false, result))
      return result;
  }

  // x is inf or nan.
  if (unlikely(x_abs >= 0x7f80'0000U)) {
    if (x_abs == 0x7f80'0000U) {
      errno = EDOM;
      fputil::set_except(FE_INVALID);
    }
    return x +
           FPBits::build_nan(1 << (fputil::MantissaWidth<float>::VALUE - 1));
  }

  // Combine the results with the sine of sum formula:
  //   cos(x) = cos((k + y)*pi/16)
  //          = cos(y*pi/16) * cos(k*pi/16) - sin(y*pi/16) * sin(k*pi/16)
  //          = cosm1_y * cos_k + sin_y * sin_k
  //          = (cosm1_y * cos_k + cos_k) + sin_y * sin_k
  double sin_k, cos_k, sin_y, cosm1_y;

  sincosf_eval(xd, x_abs, sin_k, cos_k, sin_y, cosm1_y);

  return fputil::multiply_add(sin_y, -sin_k,
                              fputil::multiply_add(cosm1_y, cos_k, cos_k));
}

} // namespace __llvm_libc
