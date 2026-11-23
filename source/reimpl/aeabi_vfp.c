/*
 * Fase 73: hardware-VFP replacements for the ARM EABI soft-float helpers.
 *
 * libGangster2.so is armeabi (ARMv6, soft-float): EVERY float/double add,
 * mul, div, compare and int<->float conversion in the engine (physics,
 * animation, CSkinnedMesh software skinning, matrices, camera) is a call to
 * __aeabi_fmul/__aeabi_fadd/... (see CColladaSoftwareSkinTechnique::skin in
 * out_ghidra.c: the vertex loop is nothing but __aeabi_fmul/__aeabi_fadd).
 * dynlib.c resolved them against the toolchain's libgcc, whose versions are
 * the generic integer-only emulation (ieee754-sf.S / ieee754-df.S): tens of
 * instructions per operation on the render thread (core 0).
 *
 * This file is built with -mfloat-abi=softfp (the project-wide flag), which
 * is exactly the __aeabi_* calling convention (args/results in r0-r3), while
 * the body compiles to a single VFP instruction. It MUST be built without
 * -ffast-math (see CMakeLists.txt) so NaN compares keep IEEE semantics.
 *
 * Not replaced (no VFP instruction, libgcc is fine): 64-bit integer <->
 * float conversions (l2f, ul2f, l2d, ul2d, f2lz, f2ulz, d2lz, d2ulz) and the
 * integer divisions (Cortex-A9 has no hardware divide).
 */

#include "reimpl/aeabi_vfp.h"

float vfp_fadd(float a, float b) { return a + b; }
float vfp_fsub(float a, float b) { return a - b; }
float vfp_fmul(float a, float b) { return a * b; }
float vfp_fdiv(float a, float b) { return a / b; }
int vfp_fcmpeq(float a, float b) { return a == b; }
int vfp_fcmplt(float a, float b) { return a < b; }
int vfp_fcmple(float a, float b) { return a <= b; }
int vfp_fcmpge(float a, float b) { return a >= b; }
int vfp_fcmpgt(float a, float b) { return a > b; }
int vfp_f2iz(float a) { return (int)a; }
float vfp_i2f(int a) { return (float)a; }
float vfp_ui2f(unsigned a) { return (float)a; }
double vfp_f2d(float a) { return (double)a; }
float vfp_d2f(double a) { return (float)a; }

double vfp_dadd(double a, double b) { return a + b; }
double vfp_dsub(double a, double b) { return a - b; }
double vfp_dmul(double a, double b) { return a * b; }
double vfp_ddiv(double a, double b) { return a / b; }
int vfp_dcmpeq(double a, double b) { return a == b; }
int vfp_dcmplt(double a, double b) { return a < b; }
int vfp_dcmple(double a, double b) { return a <= b; }
int vfp_dcmpge(double a, double b) { return a >= b; }
int vfp_dcmpgt(double a, double b) { return a > b; }
int vfp_dcmpun(double a, double b) { return __builtin_isunordered(a, b); }
int vfp_d2iz(double a) { return (int)a; }
unsigned vfp_d2uiz(double a) { return (unsigned)a; }
double vfp_i2d(int a) { return (double)a; }
double vfp_ui2d(unsigned a) { return (double)a; }

/* Boot self-check against values whose IEEE results are exact; returns the
 * number of mismatches (0 = OK). Called once from main before the .so runs. */
int aeabi_vfp_selftest(void) {
    volatile float nan = __builtin_nanf("");
    volatile double dnan = __builtin_nan("");
    int bad = 0;
    bad += vfp_fmul(1.5f, -2.0f) != -3.0f;
    bad += vfp_fadd(0.25f, 0.5f) != 0.75f;
    bad += vfp_fdiv(1.0f, 4.0f) != 0.25f;
    bad += vfp_fcmpeq(nan, nan) != 0;
    bad += vfp_fcmplt(nan, 1.0f) != 0 || vfp_fcmpge(nan, 1.0f) != 0;
    bad += vfp_f2iz(-2.75f) != -2;
    bad += vfp_i2f(-7) != -7.0f;
    bad += vfp_dmul(3.0, 0.5) != 1.5;
    bad += vfp_dcmpun(dnan, 1.0) != 1 || vfp_dcmpun(1.0, 2.0) != 0;
    bad += vfp_d2uiz(3.9) != 3u;
    return bad;
}
