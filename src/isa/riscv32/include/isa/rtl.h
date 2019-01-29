#ifndef __RISCV32_RTL_H__
#define __RISCV32_RTL_H__

#include "cpu/rtl.h"

static inline void rtl_lr(rtlreg_t* dest, int r, int width) {
  if (r != 0) { rtl_mv(dest, &reg_l(r)); }
  else { rtl_li(dest, 0); }
}

static inline void rtl_sr(int r, const rtlreg_t *src1, int width) {
  if (r != 0) { rtl_mv(&reg_l(r), src1); }
}

#endif
