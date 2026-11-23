#ifndef AEABI_VFP_H
#define AEABI_VFP_H

/* Hardware-VFP versions of the __aeabi_* soft-float helpers the .so imports.
 * See aeabi_vfp.c. */
float vfp_fadd(float a, float b);
float vfp_fsub(float a, float b);
float vfp_fmul(float a, float b);
float vfp_fdiv(float a, float b);
int vfp_fcmpeq(float a, float b);
int vfp_fcmplt(float a, float b);
int vfp_fcmple(float a, float b);
int vfp_fcmpge(float a, float b);
int vfp_fcmpgt(float a, float b);
int vfp_f2iz(float a);
float vfp_i2f(int a);
float vfp_ui2f(unsigned a);
double vfp_f2d(float a);
float vfp_d2f(double a);
double vfp_dadd(double a, double b);
double vfp_dsub(double a, double b);
double vfp_dmul(double a, double b);
double vfp_ddiv(double a, double b);
int vfp_dcmpeq(double a, double b);
int vfp_dcmplt(double a, double b);
int vfp_dcmple(double a, double b);
int vfp_dcmpge(double a, double b);
int vfp_dcmpgt(double a, double b);
int vfp_dcmpun(double a, double b);
int vfp_d2iz(double a);
unsigned vfp_d2uiz(double a);
double vfp_i2d(int a);
double vfp_ui2d(unsigned a);

int aeabi_vfp_selftest(void);

#endif
