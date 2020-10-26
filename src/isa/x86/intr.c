#include <cpu/exec.h>
#include "monitor/difftest.h"
#include "local-include/rtl.h"

typedef union GateDescriptor {
  struct {
    uint32_t offset_15_0      : 16;
    uint32_t dont_care0       : 16;
    uint32_t dont_care1       : 15;
    uint32_t present          : 1;
    uint32_t offset_31_16     : 16;
  };
  uint32_t val;
} GateDesc;

#ifndef __ICS_EXPORT
void raise_intr(DecodeExecState *s, uint32_t NO, vaddr_t ret_addr) {
  assert(NO < 256);
  int old_cs = cpu.sreg[SR_CS].val;
  // fetch the gate descriptor with ring 0
  cpu.sreg[SR_CS].rpl = 0;
  cpu.mem_exception = 0;

  rtl_li(s, s0, cpu.idtr.base);
  rtl_lm(s, s1, s0, NO << 3, 4);
  rtl_lm(s, s0, s0, (NO << 3) + 4, 4);

  // check the present bit
  assert((*s0 >> 15) & 0x1);

  uint16_t new_cs = *s1 >> 16;

  // calcualte target address
  rtl_andi(s, s1, s1, 0xffff);
  rtl_andi(s, s0, s0, 0xffff0000);
  rtl_or(s, s1, s1, s0);

#ifndef __PA__
  if ((new_cs & 0x3) < (old_cs & 0x3)) {
    // stack switch
    assert(cpu.sreg[SR_TR].ti == 0); // check the table bit
    assert((old_cs & 0x3) == 3); // only support switching from ring 3
    assert((new_cs & 0x3) == 0); // only support switching to ring 0

    uint32_t esp3 = cpu.esp;
    uint32_t ss3  = cpu.sreg[SR_SS].val;
    cpu.esp = vaddr_read(cpu.sreg[SR_TR].base + 4, 4);
    cpu.sreg[SR_SS].val = vaddr_read(cpu.sreg[SR_TR].base + 8, 2);

    rtl_li(s, s0, ss3);
    rtl_push(s, s0);
    rtl_li(s, s0, esp3);
    rtl_push(s, s0);
  }
#endif

  void rtl_compute_eflags(DecodeExecState *s, rtlreg_t *dest);
  rtl_compute_eflags(s, s0);
  rtl_push(s, s0);
  rtl_li(s, s0, old_cs);
  rtl_push(s, s0);   // cs
  rtl_li(s, s0, ret_addr);
  rtl_push(s, s0);

#ifndef __PA__
  if (NO == 14) {
    // page fault has error code
    rtl_li(s, s0, cpu.error_code);
    rtl_push(s, s0);
  }
#endif

  rtl_mv(s, &cpu.IF, rz);
  cpu.sreg[SR_CS].val = new_cs;

  rtl_jr(s, s1);

#ifdef DIFF_TEST
  if (ref_difftest_raise_intr) ref_difftest_raise_intr(NO);
#endif
}

#ifdef __PA__
#define IRQ_TIMER 32
#else
#define IRQ_TIMER 48
#endif

void query_intr(DecodeExecState *s) {
  if (cpu.INTR && cpu.IF) {
    cpu.INTR = false;
    raise_intr(s, IRQ_TIMER, cpu.pc);
    update_pc(s);
  }
}
#else
void raise_intr(DecodeExecState *s, uint32_t NO, vaddr_t ret_addr) {
  /* TODO: Trigger an interrupt/exception with ``NO''.
   * That is, use ``NO'' to index the IDT.
   */

  TODO();
}

void query_intr(DecodeExecState *s) {
  TODO();
}
#endif
