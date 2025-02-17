/***************************************************************************************
* Copyright (c) 2014-2021 Zihao Yu, Nanjing University
* Copyright (c) 2020-2022 Institute of Computing Technology, Chinese Academy of Sciences
*
* NEMU is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/

#include <isa.h>
#include <memory/vaddr.h>
#include <memory/paddr.h>
#include <memory/host.h>
#include <cpu/cpu.h>
#include "../local-include/csr.h"
#include "../local-include/intr.h"

typedef union PageTableEntry {
  struct {
    uint32_t v   : 1;
    uint32_t r   : 1;
    uint32_t w   : 1;
    uint32_t x   : 1;
    uint32_t u   : 1;
    uint32_t g   : 1;
    uint32_t a   : 1;
    uint32_t d   : 1;
    uint32_t rsw : 2;
    uint64_t ppn :44;
    uint32_t pad :10;
  };
  uint64_t val;
} PTE;

#define PGSHFT 12
#define PGMASK ((1ull << PGSHFT) - 1)
#define PGBASE(pn) (pn << PGSHFT)

// Sv39 page walk
#define PTW_LEVEL 3
#define PTE_SIZE 8
#define VPNMASK 0x1ff
static inline uintptr_t VPNiSHFT(int i) {
  return (PGSHFT) + 9 * i;
}
static inline uintptr_t VPNi(vaddr_t va, int i) {
  return (va >> VPNiSHFT(i)) & VPNMASK;
}

static inline bool check_permission(PTE *pte, bool ok, vaddr_t vaddr, int type) {
  bool ifetch = (type == MEM_TYPE_IFETCH);
  uint32_t mode = (mstatus->mprv && !ifetch ? mstatus->mpp : cpu.mode);
  assert(mode == MODE_U || mode == MODE_S);
  ok = ok && pte->v;
  ok = ok && !(mode == MODE_U && !pte->u);
  Logtr("ok: %i, mode == U: %i, pte->u: %i, ppn: %lx", ok, mode == MODE_U, pte->u, (uint64_t)pte->ppn << 12);
  ok = ok && !(pte->u && ((mode == MODE_S) && (!mstatus->sum || ifetch)));
  if (ifetch) {
    Logtr("Translate for instr reading");
#ifdef CONFIG_SHARE
//  update a/d by exception
    bool update_ad = !pte->a;
    if (update_ad && ok && pte->x)
      Logtr("raise exception to update ad for ifecth");
#else
    bool update_ad = false;
#endif
    if (!(ok && pte->x) || update_ad) {
      assert(!cpu.amo);
      INTR_TVAL_REG(EX_IPF) = vaddr;
      longjmp_exception(EX_IPF);
      return false;
    }
  } else if (type == MEM_TYPE_READ) {
    Logtr("Translate for memory reading");
    bool can_load = pte->r || (mstatus->mxr && pte->x);
#ifdef CONFIG_SHARE
    bool update_ad = !pte->a;
    if (update_ad && ok && can_load)
      Logtr("raise exception to update ad for load");
#else
    bool update_ad = false;
#endif
    if (!(ok && can_load) || update_ad) {
      if (cpu.amo) Logtr("redirect to AMO page fault exception at pc = " FMT_WORD, cpu.pc);
      int ex = (cpu.amo ? EX_SPF : EX_LPF);
      INTR_TVAL_REG(ex) = vaddr;
      cpu.amo = false;
      Logtr("Memory read translation exception!");
      longjmp_exception(ex);
      return false;
    }
  } else {
#ifdef CONFIG_SHARE
    bool update_ad = !pte->a || !pte->d;
   if (update_ad && ok && pte->w) Logtr("raise exception to update ad for store");
#else
    bool update_ad = false;
#endif
    Logtr("Translate for memory writing");
    if (!(ok && pte->w) || update_ad) {
      INTR_TVAL_REG(EX_SPF) = vaddr;
      cpu.amo = false;
      longjmp_exception(EX_SPF);
      return false;
    }
  }
  return true;
}

static paddr_t ptw(vaddr_t vaddr, int type) {
  Logtr("Page walking for 0x%lx\n", vaddr);
  word_t pg_base = PGBASE(satp->ppn);
  word_t p_pte; // pte pointer
  PTE pte;
  int level;
  int64_t vaddr39 = vaddr << (64 - 39);
  vaddr39 >>= (64 - 39);
  if ((uint64_t)vaddr39 != vaddr) goto bad;
  for (level = PTW_LEVEL - 1; level >= 0;) {
    p_pte = pg_base + VPNi(vaddr, level) * PTE_SIZE;
#ifdef CONFIG_MULTICORE_DIFF
    pte.val = golden_pmem_read(p_pte, PTE_SIZE, 0, 0, 0);
#else
    pte.val	= paddr_read(p_pte, PTE_SIZE,
      type == MEM_TYPE_IFETCH ? MEM_TYPE_IFETCH_READ :
      type == MEM_TYPE_WRITE ? MEM_TYPE_WRITE_READ : MEM_TYPE_READ, MODE_S, vaddr);
#endif
#ifdef CONFIG_SHARE
    if (unlikely(dynamic_config.debug_difftest)) {
      fprintf(stderr, "[NEMU] ptw: level %d, vaddr 0x%lx, pg_base 0x%lx, p_pte 0x%lx, pte.val 0x%lx\n",
        level, vaddr, pg_base, p_pte, pte.val);
    }
#endif
    pg_base = PGBASE(pte.ppn);
    if (!pte.v || (!pte.r && pte.w)) goto bad;
    if (pte.r || pte.x) { break; }
    else {
      level --;
      if (level < 0) { goto bad; }
    }
  }

  if (!check_permission(&pte, true, vaddr, type)) return MEM_RET_FAIL;

  if (level > 0) {
    // superpage
    word_t pg_mask = ((1ull << VPNiSHFT(level)) - 1);
    if ((pg_base & pg_mask) != 0) {
      // missaligned superpage
      goto bad;
    }
    pg_base = (pg_base & ~pg_mask) | (vaddr & pg_mask & ~PGMASK);
  }

#ifndef CONFIG_SHARE
  // update a/d by hardware
  bool is_write = (type == MEM_TYPE_WRITE);
  if (!pte.a || (!pte.d && is_write)) {
    pte.a = true;
    pte.d |= is_write;
    paddr_write(p_pte, PTE_SIZE, pte.val, cpu.mode, vaddr);
  }
#endif

  return pg_base | MEM_RET_OK;

bad:
  Logtr("Memory translation bad");
  check_permission(&pte, false, vaddr, type);
  return MEM_RET_FAIL;
}

static int ifetch_mmu_state = MMU_DIRECT;
static int data_mmu_state = MMU_DIRECT;

int get_data_mmu_state() {
  return (data_mmu_state == MMU_DIRECT ? MMU_DIRECT : MMU_TRANSLATE);
}

static inline int update_mmu_state_internal(bool ifetch) {
  uint32_t mode = (mstatus->mprv && (!ifetch) ? mstatus->mpp : cpu.mode);
  if (mode < MODE_M) {
    assert(satp->mode == 0 || satp->mode == 8);
    if (satp->mode == 8) return MMU_TRANSLATE;
  }
  return MMU_DIRECT;
}

int update_mmu_state() {
  ifetch_mmu_state = update_mmu_state_internal(true);
  int data_mmu_state_old = data_mmu_state;
  data_mmu_state = update_mmu_state_internal(false);
  return (data_mmu_state ^ data_mmu_state_old) ? true : false;
}

int isa_mmu_check(vaddr_t vaddr, int len, int type) {
  Logtr("MMU checking addr %lx", vaddr);
  bool is_ifetch = type == MEM_TYPE_IFETCH;
  // riscv-privileged 4.4.1: Addressing and Memory Protection:
  // Instruction fetch addresses and load and store effective addresses,
  // which are 64 bits, must have bits 63–39 all equal to bit 38, or else a page-fault exception will occur.
  bool vm_enable = (mstatus->mprv && (!is_ifetch) ? mstatus->mpp : cpu.mode) < MODE_M && satp->mode == 8;
  word_t va_mask = ((((word_t)1) << (63 - 38 + 1)) - 1);
  word_t va_msbs = vaddr >> 38;
  bool va_msbs_ok = (va_msbs == va_mask) || va_msbs == 0 || !vm_enable;

  if(!va_msbs_ok){
    if(is_ifetch){
      stval->val = vaddr;
      INTR_TVAL_REG(EX_IPF) = vaddr;
      longjmp_exception(EX_IPF);
    } else if(type == MEM_TYPE_READ){
      int ex = cpu.amo ? EX_SPF : EX_LPF;
      INTR_TVAL_REG(ex) = vaddr;
      longjmp_exception(ex);
    } else {
      INTR_TVAL_REG(EX_SPF) = vaddr;
      longjmp_exception(EX_SPF);
    }
    return MEM_RET_FAIL;
  }

  if (is_ifetch) return ifetch_mmu_state ? MMU_TRANSLATE : MMU_DIRECT;
  if (ISDEF(CONFIG_AC_SOFT) && unlikely((vaddr & (len - 1)) != 0)) {
    Log("addr misaligned happened: vaddr:%lx len:%d type:%d pc:%lx", vaddr, len, type, cpu.pc);
    // assert(0);
    int ex = cpu.amo || type == MEM_TYPE_WRITE ? EX_SAM : EX_LAM;
    INTR_TVAL_REG(ex) = vaddr;
    longjmp_exception(ex);
    return MEM_RET_FAIL;
  }
  return data_mmu_state ? MMU_TRANSLATE : MMU_DIRECT;
}

#ifdef CONFIG_SHARE
void isa_misalign_data_addr_check(vaddr_t vaddr, int len, int type) {
  if (ISDEF(CONFIG_AC_SOFT) && unlikely((vaddr & (len - 1)) != 0)) {
    int ex = cpu.amo || type == MEM_TYPE_WRITE ? EX_SAM : EX_LAM;
    INTR_TVAL_REG(ex) = vaddr;
    longjmp_exception(ex);
  }
}
#endif

paddr_t isa_mmu_translate(vaddr_t vaddr, int len, int type) {
  paddr_t ptw_result = ptw(vaddr, type);
#ifdef FORCE_RAISE_PF
  if(ptw_result != MEM_RET_FAIL && force_raise_pf(vaddr, type) != MEM_RET_OK)
    return MEM_RET_FAIL;
#endif
  return ptw_result;
}

int force_raise_pf_record(vaddr_t vaddr, int type) {
  static vaddr_t last_addr[3] = {0x0};
  static int force_count[3] = {0};
  if (vaddr != last_addr[type]) {
    last_addr[type] = vaddr;
    force_count[type] = 0;
  }
  force_count[type]++;
  return force_count[type] == 5;
}

int force_raise_pf(vaddr_t vaddr, int type){
  bool ifetch = (type == MEM_TYPE_IFETCH);

  if(cpu.guided_exec && cpu.execution_guide.force_raise_exception){
    if(ifetch && cpu.execution_guide.exception_num == EX_IPF){
      if (force_raise_pf_record(vaddr, type)) {
        return MEM_RET_OK;
      }
      if (!intr_deleg_S(EX_IPF)) {
        mtval->val = cpu.execution_guide.mtval;
        if(
          vaddr != cpu.execution_guide.mtval &&
          // cross page ipf caused mismatch is legal
          !((vaddr & 0xfff) == 0xffe && (cpu.execution_guide.mtval & 0xfff) == 0x000)
        ){
          printf("[WARNING] nemu mtval %lx does not match core mtval %lx\n",
            vaddr,
            cpu.execution_guide.mtval
          );
        }
      } else {
        stval->val = cpu.execution_guide.stval;
        if(
          vaddr != cpu.execution_guide.stval &&
          // cross page ipf caused mismatch is legal
          !((vaddr & 0xfff) == 0xffe && (cpu.execution_guide.stval & 0xfff) == 0x000)
        ){
          printf("[WARNING] nemu stval %lx does not match core stval %lx\n",
            vaddr,
            cpu.execution_guide.stval
          );
        }
      }
      printf("force raise IPF\n");
      longjmp_exception(EX_IPF);
      return MEM_RET_FAIL;
    } else if(!ifetch && type == MEM_TYPE_READ && cpu.execution_guide.exception_num == EX_LPF){
      if (force_raise_pf_record(vaddr, type)) {
        return MEM_RET_OK;
      }
      INTR_TVAL_REG(EX_LPF) = vaddr;
      printf("force raise LPF\n");
      longjmp_exception(EX_LPF);
      return MEM_RET_FAIL;
    } else if(type == MEM_TYPE_WRITE && cpu.execution_guide.exception_num == EX_SPF){
      if (force_raise_pf_record(vaddr, type)) {
        return MEM_RET_OK;
      }
      INTR_TVAL_REG(EX_SPF) = vaddr;
      printf("force raise SPF\n");
      longjmp_exception(EX_SPF);
      return MEM_RET_FAIL;
    }
  }
  return MEM_RET_OK;
}

#ifndef CONFIG_RV_SPMP_CHECK
#ifndef CONFIG_PMPTABLE_EXTENSION
#define DISABLE_ADDR_MATCHING_FOR_SPMP_AND_PMPTABLE
#endif
#endif

#ifndef DISABLE_ADDR_MATCHING_FOR_SPMP_AND_PMPTABLE
static bool napot_decode(paddr_t addr, word_t spmp_addr) {
  word_t spmp_addr_start, spmp_addr_end;
  spmp_addr_start = (spmp_addr & (spmp_addr + 1)) << SPMP_SHIFT;
  spmp_addr_end = (spmp_addr | (spmp_addr + 1)) << SPMP_SHIFT;
  return ((spmp_addr_start <= addr && addr < spmp_addr_end) ? true : false);
}

static uint8_t address_matching(paddr_t base, paddr_t addr, int len, word_t spmp_addr, uint8_t addr_mode) {
  paddr_t addr_s, addr_e;
  addr_s = addr;
  addr_e = addr + len;
  uint8_t s_flag = 0;
  uint8_t e_flag = 0;

  if (addr_mode == SPMP_TOR) {
    spmp_addr = spmp_addr << SPMP_SHIFT;
    s_flag = (base <= addr_s && addr_s < spmp_addr ) ? 1 : 0;
    e_flag = (base <= addr_e && addr_e < spmp_addr) ? 1 : 0;
  }
  else if (addr_mode == SPMP_NA4) {
    spmp_addr = spmp_addr << SPMP_SHIFT;
    s_flag = (spmp_addr <= addr_s && addr_s < (spmp_addr + (1 << SPMP_SHIFT))) ? 1 : 0;
    e_flag = (spmp_addr <= addr_e && addr_e < (spmp_addr + (1 << SPMP_SHIFT))) ? 1 : 0;
  }
  else if (addr_mode == SPMP_NAPOT) {
    s_flag = napot_decode(addr_s, spmp_addr) ? 1 : 0;
    e_flag = napot_decode(addr_e, spmp_addr) ? 1 : 0;
  }
  return s_flag + e_flag;
}
#endif

#ifdef CONFIG_PMPTABLE_EXTENSION
bool pmpcfg_check_permission(uint8_t pmpcfg,int type,int out_mode) {
  if (out_mode == MODE_M) {
    return true;
  }
  else {
    if (type == MEM_TYPE_READ || type == MEM_TYPE_IFETCH_READ ||
        type == MEM_TYPE_WRITE_READ)
      return pmpcfg & PMP_R;
    else if (type == MEM_TYPE_WRITE)
      return pmpcfg & PMP_W;
    else if (type == MEM_TYPE_IFETCH)
      return pmpcfg & PMP_X;
    else {
      Log("Wrong Type: %d!", type);
      return false;
    }
  }
}

bool pmptable_check_permission(word_t offset, word_t root_table_base, int type, int out_mode) {
  if (out_mode == MODE_M) {
    return true;
  }
  else {
    uint64_t off1 = (offset >> 25) & 0x1ff; /* root offset */
    uint64_t off0 = (offset >> 16) & 0x1ff; /* leaf offset */
    uint8_t page_index = (offset >> 12) & 0xf;  /* page index */
    uint8_t perm = 0;

    // Log("root_pte_base is: %#lx.", root_table_base);
    uint64_t root_pte_addr = root_table_base + (off1 << 3);
    // Log("root_pte_addr is: %#lx.", root_pte_addr);
    uint64_t root_pte = host_read(guest_to_host(root_pte_addr), 8);
    // Log("root_pte is: %#lx.", root_pte);

    // Log("root_pte_addr is: %#lx.", root_pte_addr);
    // Log("root_pte is: %#lx.", root_pte);
    // Log("flag is: %ld.", root_pte & 0x0f);
    // Log("off1 is: %#lx.", off1);

    if ((root_pte & 0x0f) == 1) {
      bool at_high = page_index % 2;
      int idx = page_index / 2;
      uint8_t leaf_pte = host_read(guest_to_host(((root_pte >> 5) << 12) + (off0 << 3)) + idx, 1);
      Log("hit leaf pte: %#lx.", (uint64_t)leaf_pte);
      if (at_high) {
        perm = leaf_pte >> 4;
      } 
      else {
        perm = leaf_pte & 0xf;
      }
    }
    else if ((root_pte & 0x1) == 1) {
      perm = (root_pte >> 1) & 0xf;
    }

    perm = ((perm & 0x3) == 0x2) ? (perm & 0x4) : perm;

#define R_BIT 0x1
#define W_BIT 0x2
#define X_BIT 0x4
    if (type == MEM_TYPE_READ || type == MEM_TYPE_IFETCH_READ || type == MEM_TYPE_WRITE_READ) {
      return perm & R_BIT;
    }
    else if (type == MEM_TYPE_WRITE) {
      return perm & W_BIT;
    }
    else if (type == MEM_TYPE_IFETCH) {
      return perm & X_BIT;
    }
    else {
      Log("pmptable get wrong type of memory access!");
      return false;
    }
#undef R_BIT
#undef W_BIT 
#undef X_BIT 
  }
}
#endif

bool isa_pmp_check_permission(paddr_t addr, int len, int type, int out_mode) {
  bool ifetch = (type == MEM_TYPE_IFETCH);
  __attribute__((unused)) uint32_t mode;
  mode = (out_mode == MODE_M) ? (mstatus->mprv && !ifetch ? mstatus->mpp : cpu.mode) : out_mode;
  // paddr_read/write method may not be able pass down the 'effective' mode for isa difference. do it here
#ifdef CONFIG_SHARE
  // if(dynamic_config.debug_difftest) {
  //   if (mode != out_mode) {
  //     fprintf(stderr, "[NEMU]   PMP out_mode:%d cpu.mode:%ld ifetch:%d mprv:%d mpp:%d actual mode:%d\n", out_mode, cpu.mode, ifetch, mstatus->mprv, mstatus->mpp, mode);
  //       // Log("addr:%lx len:%d type:%d out_mode:%d mode:%d", addr, len, type, out_mode, mode);
  //   }
  // }
#endif

#ifdef CONFIG_RV_PMP_CHECK
  if (CONFIG_RV_PMP_NUM == 0) {
    return true;
  }

  word_t base = 0;
  for (int i = 0; i < CONFIG_RV_PMP_NUM; i++) {
    word_t pmpaddr = pmpaddr_from_index(i);
    word_t tor = (pmpaddr & pmp_tor_mask()) << PMP_SHIFT;
    uint8_t cfg = pmpcfg_from_index(i);

    if (cfg & PMP_A) {
      bool is_tor = (cfg & PMP_A) == PMP_TOR;
      bool is_na4 = (cfg & PMP_A) == PMP_NA4;

      word_t mask = (pmpaddr << 1) | (!is_na4) | ~pmp_tor_mask();
      mask = ~(mask & ~(mask + 1)) << PMP_SHIFT;

      // Check each 4-byte sector of the access
      bool any_match = false;
      bool all_match = true;
      for (word_t offset = 0; offset < len; offset += 1 << PMP_SHIFT) {
        word_t cur_addr = addr + offset;
        bool napot_match = ((cur_addr ^ tor) & mask) == 0;
        bool tor_match = base <= cur_addr && cur_addr < tor;
        bool match = is_tor ? tor_match : napot_match;
        any_match |= match;
        all_match &= match;
#ifdef CONFIG_SHARE
        // if(dynamic_config.debug_difftest) {
        //   fprintf(stderr, "[NEMU]   PMP byte match %ld addr:%016lx cur_addr:%016lx tor:%016lx mask:%016lx base:%016lx match:%s\n",
        //   offset, addr, cur_addr, tor, mask, base, match ? "true" : "false");
        // }
#endif
      }
#ifdef CONFIG_SHARE
        // if(dynamic_config.debug_difftest) {
        //   fprintf(stderr, "[NEMU]   PMP %d cfg:%02x pmpaddr:%016lx isna4:%d isnapot:%d istor:%d base:%016lx addr:%016lx any_match:%d\n",
        //     i, cfg, pmpaddr, is_na4, !is_na4 && !is_tor, is_tor, base, addr, any_match);
        // }
#endif
      if (any_match) {
        // If the PMP matches only a strict subset of the access, fail it
        if (!all_match) {
#ifdef CONFIG_SHARE
          // if(dynamic_config.debug_difftest) {
          //   fprintf(stderr, "[NEMU]   PMP addr:0x%016lx len:%d type:%d mode:%d pass:false for not all match\n", addr, len, type, mode);
          // }
#endif
          return false;
        }

#ifdef CONFIG_SHARE
        // if(dynamic_config.debug_difftest) {
        //   bool pass = (mode == MODE_M && !(cfg & PMP_L)) ||
        //       ((type == MEM_TYPE_READ || type == MEM_TYPE_IFETCH_READ ||
        //         type == MEM_TYPE_WRITE_READ) && (cfg & PMP_R)) ||
        //       (type == MEM_TYPE_WRITE && (cfg & PMP_W)) ||
        //       (type == MEM_TYPE_IFETCH && (cfg & PMP_X));
        //   fprintf(stderr, "[NEMU]   PMP %d cfg:%02x pmpaddr:%016lx addr:0x%016lx len:%d type:%d mode:%d pass:%s \n", i, cfg, pmpaddr, addr, len, type, mode,
        //       pass ? "true" : "false for permission denied");
        // }
#endif

        return
          (mode == MODE_M && !(cfg & PMP_L)) ||
          ((type == MEM_TYPE_READ || type == MEM_TYPE_IFETCH_READ ||
            type == MEM_TYPE_WRITE_READ) && (cfg & PMP_R)) ||
          (type == MEM_TYPE_WRITE && (cfg & PMP_W)) ||
          (type == MEM_TYPE_IFETCH && (cfg & PMP_X));
      }
    }

    base = tor;
  }

#ifdef CONFIG_SHARE
  // if(dynamic_config.debug_difftest) {
  //   if (mode != MODE_M) fprintf(stderr, "[NEMU]   PMP addr:0x%016lx len:%d type:%d mode:%d pass:%s\n", addr, len, type, mode,
  //   mode == MODE_M ? "true for mode m but no match" : "false for no match with less than M mode");
  // }
#endif

  return mode == MODE_M;

#endif

#ifdef CONFIG_PMPTABLE_EXTENSION
  if (CONFIG_RV_PMP_NUM == 0) {
    return true;
  }

  int i = 0;
  word_t base = 0;
  for (i = 0; i < CONFIG_RV_PMP_NUM; i++) {
    uint8_t pmpcfg = pmpcfg_from_index(i);
    word_t pmpaddr = pmpaddr_from_index(i);
    uint8_t addr_mode = pmpcfg & PMP_A;
    if (addr_mode) {
      int match_ret = 0;
      match_ret = address_matching(base, addr, len, pmpaddr, addr_mode);
      /* when_ret == 1, means that addr is half in a pmpaddr region
       * and it is illegal.
       */
      if (match_ret == 1) {
        Log("[ERROR] addr is misaligned in pmp check. pmpcfg[%d] = %#x", i, pmpcfg);
        return false;
      }
      else if (match_ret == 0){
        continue;
      }
      else {
        if (pmpcfg & PMP_T) {
          // Log("[INFO] pmpcfg[%d] is %#2x, pmptable used.", i, pmpcfg);
          word_t offset = 0;
          if (addr_mode == PMP_TOR){
            offset = addr - base;
          }
          else {
            offset = addr - (pmpaddr << PMP_SHIFT);
          }
          word_t root_table_base = pmpaddr_from_index(i + 1) << 12;
          if (addr == 0xc0000000) {
            Log("addr = %#lx, catch the bug.", addr);
            Log("offset = %#lx, catch the bug.", offset);
            Log("base = %#lx, important value.", base);
            Log("root_table_base = %#lx, catch the bug.", root_table_base);
          }
          return pmptable_check_permission(offset, root_table_base, type, out_mode);
        }
        else {
          // Log("[INFO] pmpcfg[%d] is %#2x, pmptable not used.\n", i, pmpcfg);
          return pmpcfg_check_permission(pmpcfg, type, out_mode);
        }
      }
    }
    base = pmpaddr << PMP_SHIFT;
  }
  return true;
#endif

#ifndef CONFIG_RV_PMP_CHECK
#ifndef CONFIG_PMPTABLE_EXTENSION
  return true;
#endif
#endif
}

#ifdef CONFIG_RV_SPMP_CHECK

static bool spmp_internal_check_permission(uint8_t spmp_cfg, int type, int out_mode) {
  uint8_t spmp_permission, permission_ret;  // ret R/W/X
  spmp_permission = ((spmp_cfg & SPMP_S) >> 4) | (spmp_cfg & SPMP_R) << 2 | (spmp_cfg & SPMP_W) | ((spmp_cfg & SPMP_X) >> 2); // input S/R/W/X
  if (out_mode == MODE_S) {
    if (!mstatus->sum) {
      switch(spmp_permission) {
        case 0b0010: 
        case 0b0011: permission_ret = 0b110; break;
        case 0b1001: 
        case 0b1010: permission_ret = 0b001; break;
        case 0b1000: permission_ret = 0b111; break;
        case 0b1011: permission_ret = 0b101; break;
        case 0b1100: permission_ret = 0b100; break;
        case 0b1101: permission_ret = 0b101; break;
        case 0b1110: permission_ret = 0b110; break;
        case 0b1111: permission_ret = 0b100; break;
        default: permission_ret = 0b000; break;
      }
    }
    else {
      switch(spmp_permission) {
        case 0b0010: 
        case 0b0011: permission_ret = 0b110; break;
        case 0b0100: 
        case 0b0101: permission_ret = 0b100; break;
        case 0b0110:
        case 0b0111: permission_ret = 0b110; break;
        case 0b1001: 
        case 0b1010: permission_ret = 0b001; break;
        case 0b1011: permission_ret = 0b101; break;
        case 0b1000: permission_ret = 0b111; break;
        case 0b1100: permission_ret = 0b100; break;
        case 0b1101: permission_ret = 0b101; break;
        case 0b1110: permission_ret = 0b110; break;
        case 0b1111: permission_ret = 0b100; break;
        default: permission_ret = 0b000; break;
      }
    }
  }
  else if (out_mode == MODE_U) {
   switch (spmp_permission) {
    case 0b0001: permission_ret = 0b001; break;
    case 0b0010: permission_ret = 0b100; break;
    case 0b0011: permission_ret = 0b110; break;
    case 0b0100: permission_ret = 0b100; break;
    case 0b0101: permission_ret = 0b101; break;
    case 0b0110: permission_ret = 0b110; break;
    case 0b1000:
    case 0b0111: permission_ret = 0b111; break;
    case 0b1010:
    case 0b1011: permission_ret = 0b001; break;
    case 0b1111: permission_ret = 0b100; break;
    default: permission_ret = 0b000; break;
   }
  } 
  else { // MODE_M
    permission_ret = 0b111;
  }
  switch (type) {
    case MEM_TYPE_IFETCH: 
      return ((permission_ret | 0b001) == permission_ret);
    case MEM_TYPE_READ:
    case MEM_TYPE_IFETCH_READ:
    case MEM_TYPE_WRITE_READ:
      return ((permission_ret | 0b100) == permission_ret);
    case MEM_TYPE_WRITE:
      return ((permission_ret | 0b010) == permission_ret);
    default:
      return false;
  }
}
#endif


bool isa_spmp_check_permission(paddr_t addr, int len, int type, int out_mode) {
#ifdef CONFIG_RV_SPMP_CHECK
  word_t base = 0;
  for (int i = 0; i < CONFIG_RV_SPMP_NUM; i++) {
    word_t spmp_addr = spmpaddr_from_index(i);
    uint8_t spmp_cfg = spmpcfg_from_index(i);
    uint8_t addr_mode = spmp_cfg & PMP_A;
    if (addr_mode != 0) {
      uint8_t matching_result = 0;
      matching_result = address_matching(base, addr, len, spmp_addr, addr_mode); 
      if (matching_result == 1)
      {
        printf("spmp addr misalianed!\n");
        return false;
      }
      else if (matching_result == 0){
        continue;
      }
      else {
        return spmp_internal_check_permission(spmp_cfg, type, out_mode);
      }
    }
    base = spmp_addr << SPMP_SHIFT;
  }
  // no matching --> true or false??
  if (out_mode == MODE_U) {
    // printf("spmp Mode U refuse!\n");
    return true;
  }
  else {
    return true;
  }
#else
  return true;
#endif
}