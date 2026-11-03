/*
 * Compile-time guards for the floating-point determinism assumptions
 * (docs/design.md, "Fixed-point arithmetic and determinism").
 *
 * The init-time double code is only bit-identical across targets if double
 * expressions are evaluated in double and a*b + c is never fused. Include
 * this header first in every source file that does floating-point
 * arithmetic, so a build that breaks either assumption fails to compile
 * instead of silently changing the output.
 */
#ifndef EPI_FP_H
#define EPI_FP_H

#include <float.h>

/* Excess precision (32-bit x87, -ffast-math, -ffp-eval-method=...). 16 is
 * GCC's value for targets with native _Float16: float and double are still
 * evaluated at their own precision. */
#if !defined(FLT_EVAL_METHOD) || (FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != 16)
#error                                                                                                                 \
    "epaper-image: floating-point expressions must be evaluated at their own precision (FLT_EVAL_METHOD 0); output would not be deterministic (x87 excess precision or -ffast-math?)"
#endif

/* Fused multiply-add contraction. Clang defines __GNUC__ too: test it first. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#elif defined(__GNUC__)
/* GCC warns about the STDC pragma, so it is checked rather than set. */
#if defined(__FP_FAST_FMA) && !defined(__STRICT_ANSI__) && !defined(EPI_FP_CONTRACT_OFF)
#error                                                                                                                 \
    "epaper-image: this target fuses a*b + c, which would make output non-deterministic; compile with -ffp-contract=off and define EPI_FP_CONTRACT_OFF"
#endif
#endif

#endif /* EPI_FP_H */
