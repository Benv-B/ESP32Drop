#pragma once
// Host-only interface subset used by iris_image.cpp. The test supplies fake
// prepare/decomp functions, NOT a JPEG codec or a claim about the real C5 ROM.
#include <stdint.h>
typedef unsigned int UINT;
typedef uint8_t BYTE;
struct JDEC {
  void *device;
  UINT width, height;
  UINT (*infunc)(JDEC *, BYTE *, UINT);
};
struct JRECT { uint16_t left, right, top, bottom; };
enum JRESULT { JDR_OK, JDR_INTR, JDR_INP, JDR_MEM1, JDR_MEM2, JDR_PAR, JDR_FMT1 };
JRESULT jd_prepare(JDEC *, UINT(*)(JDEC *, BYTE *, UINT), void *, UINT, void *);
JRESULT jd_decomp(JDEC *, UINT(*)(JDEC *, void *, JRECT *), BYTE);
