#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[]; // defined by kernel.ld
pde_t *kpgdir;      // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X | STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc) {
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if (*pde & PTE_P)
  {
    pgtab = (pte_t *)P2V(PTE_ADDR(*pde));
  }
  else
  {
    if (!alloc || (pgtab = (pte_t *)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm) {
  char *a, *last;
  pte_t *pte;

  a = (char *)PGROUNDDOWN((uint)va);
  last = (char *)PGROUNDDOWN(((uint)va) + size - 1);
  for (;;)
  {
    if ((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if (*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap
{
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
    {(void *)KERNBASE, 0, EXTMEM, PTE_W},            // I/O space
    {(void *)KERNLINK, V2P(KERNLINK), V2P(data), 0}, // kern text+rodata
    {(void *)data, V2P(data), PHYSTOP, PTE_W},       // kern data+memory
    {(void *)DEVSPACE, DEVSPACE, 0, PTE_W},          // more devices
};

// Set up kernel part of a page table.
pde_t *setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if ((pgdir = (pde_t *)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void *)DEVSPACE)
    panic("PHYSTOP too high");
  for (k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if (mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                 (uint)k->phys_start, k->perm) < 0)
    {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void switchkvm(void)
{
  lcr3(V2P(kpgdir)); // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void switchuvm(struct proc *p)
{
  if (p == 0)
    panic("switchuvm: no process");
  if (p->kstack == 0)
    panic("switchuvm: no kstack");
  if (p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts) - 1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort)0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir)); // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W | PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if ((uint)addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, addr + i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if (sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if (readi(ip, P2V(pa), offset + i, n) != n)
      return -1;
  }
  return 0;
}

uint getEvictedVa(struct proc *p) {
  uint evictedVa = 1;

  if (PAGING_ALGO == FIFO) {
    evictedVa = p->pagesFIFO[0];
    for (int i = 0; i < p->memPageCount - 1; i++)
    {
      p->pagesFIFO[i] = p->pagesFIFO[i + 1];
      p->pagesAge[i] = p->pagesAge[i + 1];
    }
    p->memPageCount--;
  }
  else if (PAGING_ALGO == AGING) {
    uint minAge = 0xFFFFFFFF, minIndex = 0;
    for (int i = 0; i < p->memPageCount; i++)
    {
      if (p->pagesAge[i] < minAge)
      {
        minAge = p->pagesAge[i];
        minIndex = i;
      }
    }
    if (minIndex == MAX_PSYC_PAGES)
      panic("getEvictedVa: invalid minIndex");
    evictedVa = p->pagesFIFO[minIndex];
    for (int i = minIndex; i < p->memPageCount - 1; i++)
    {
      p->pagesFIFO[i] = p->pagesFIFO[i + 1];
      p->pagesAge[i] = p->pagesAge[i + 1];
    }
    p->memPageCount--;
  }
  else
    panic("getEvictedVa: unknown paging algorithm");

  return evictedVa;
}

void swapOut(struct proc *p) {
  uint evictedVa = getEvictedVa(p);

  if (evictedVa % 4096 != 0)
    panic("swapOut: invalid evictedVa");

  if( p->verbose>=1 ) {
    cprintf("Swapping Out Page %d, PID: %d\n", evictedVa>>12, p->pid);
  }

  for (int i = 0; i < MAX_SWAP_PAGES; i++)
  {
    if (p->swapMap[i] == 1)
    {
      writeToSwapFile(p, (char *)evictedVa, i * PGSIZE, PGSIZE);
      p->swapMap[i] = evictedVa;
      p->swapPageCount++;
      break;
    }
  }

  pte_t *pte = walkpgdir(p->pgdir, (char *)evictedVa, 0);
  if (pte == 0)
    panic("SWAP OUT: page to be evicted does not exist");

  uint pa = (*pte) & ~0xFFF;
  kfree(P2V(pa));
  *pte = PTE_W | PTE_U | PTE_PG;

  lcr3(V2P(p->pgdir));
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  struct proc *curproc = myproc();

  char *mem;
  uint a;

  if (newsz >= KERNBASE)
    return 0;
  if (newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for (; a < newsz; a += PGSIZE)
  {
    if (curproc->memPageCount + curproc->swapPageCount == MAX_TOTAL_PAGES)
    {
      panic("allocuvm : CANNOT ALLOCATE MORE PAGES");
    }
    if (curproc->memPageCount == MAX_PSYC_PAGES)
      swapOut(curproc);
    mem = kalloc();
    if (mem == 0)
    {
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pgdir, (char *)a, PGSIZE, V2P(mem), PTE_W | PTE_U) < 0)
    {
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
    curproc->pagesFIFO[curproc->memPageCount] = a;
    curproc->memPageCount++;
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  struct proc *p = getProcFromPgdir(pgdir);

  pte_t *pte;
  uint a, pa;

  if (newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for (; a < oldsz; a += PGSIZE)
  {
    pte = walkpgdir(pgdir, (char *)a, 0);
    if (!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if ((*pte & PTE_P) != 0)
    {
      pa = PTE_ADDR(*pte);
      if (pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      if (isUserProc(p))
      {
        for (int i = 0; i < p->memPageCount; i++)
        {
          if (p->pagesFIFO[i] == a)
          {
            for (int j = i; j < p->memPageCount - 1; j++)
            {
              p->pagesFIFO[j] = p->pagesFIFO[j + 1];
              p->pagesAge[j] = p->pagesAge[j + 1];
            }
            p->memPageCount--;
            break;
          }
        }
      }
      *pte = 0;
    }
    else if ((*pte & PTE_PG) != 0)
    {
      if (isUserProc(p))
      {
        for (int i = 0; i < MAX_SWAP_PAGES; i++)
        {
          if (p->swapMap[i] == a)
          {
            p->swapMap[i] = 1;
            p->swapPageCount--;
            break;
          }
        }
      }
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void freevm(pde_t *pgdir)
{
  uint i;

  if (pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for (i = 0; i < NPDENTRIES; i++)
  {
    if (pgdir[i] & PTE_P)
    {
      char *v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char *)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if (pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t *copyuvm(pde_t *pgdir, uint sz) {
  struct proc *p = getProcFromPgdir(pgdir);

  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if ((d = setupkvm()) == 0)
    return 0;
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walkpgdir(pgdir, (void *)i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if( isUserProc(p) ) {
      if(*pte & PTE_PG) {
        pte = walkpgdir(d, (int*)i, 1);
        *pte = PTE_PG | PTE_U | PTE_W;
        continue;
      }
    }
    if (!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char *)P2V(pa), PGSIZE);
    if (mappages(d, (void *)i, PGSIZE, V2P(mem), flags) < 0)
    {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

// void update_pageOUT_pte_flags(struct proc* p, int vAddr, pde_t * pgdir){

//   pte_t *pte = walkpgdir(pgdir, (int*)vAddr, 0);
//   if (!pte)
//     panic("update_pageOUT_pte_flags: pte does NOT exist in pgdir");

//   *pte |= PTE_PG;           // Inidicates that the page was Paged-out to secondary storage
//   *pte &= ~PTE_P;           // Indicates that the page is NOT in physical memory
//   *pte &= PTE_FLAGS(*pte);

//   lcr3(V2P(p->pgdir));      // Refresh CR3 register (TLB (cache))
// }

// PAGEBREAK!
//  Map user virtual address to kernel address.
char *
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if ((*pte & PTE_P) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  return (char *)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char *)p;
  while (len > 0)
  {
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char *)va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if (n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

void printPagingInfo(struct proc *p) {
  pde_t *pgdir_base = p->pgdir;

  cprintf("\nPage Tables:\n");
  cprintf("\tMemory Location of Page Directory = %d", V2P(pgdir_base));

  int mappings[MAX_PSYC_PAGES][2];
  int map_idx = 0;

  uint pgdir_entry_present = 0;

  uint pgdir_index, pgdir_entry, pgtable_index, pgtable_entry, pgtable_PPN, page_PPN;
  uint *pgtable_base;

  for (pgdir_index = 0; pgdir_index < 1024; pgdir_index++)
  {
    pgdir_entry = pgdir_base[pgdir_index];

    if (pgdir_entry & PTE_P)
    {
      pgtable_PPN = pgdir_entry >> 12;
      pgtable_base = P2V(pgtable_PPN << 12);

      for (pgtable_index = 0; pgtable_index < 1024; pgtable_index++)
      {
        pgtable_entry = pgtable_base[pgtable_index];

        if ((pgtable_entry & PTE_P) && (pgtable_entry & PTE_U) && !(pgtable_entry & PTE_PG))
        {

          if (pgdir_entry_present == 0)
          {
            cprintf("\n\tpgdir PTE %d, %d:", pgdir_index, pgtable_PPN);
            cprintf("\n\t\tmemory location of page table = %d", pgtable_PPN << 12);
            pgdir_entry_present = 1;
          }

          page_PPN = pgtable_entry >> 12;
          cprintf("\n\t\tptbl PTE %d, %d, %d", pgtable_index, page_PPN, page_PPN << 12);

          mappings[map_idx][0] = pgdir_index * 1024 + pgtable_index;
          mappings[map_idx++][1] = page_PPN;
        }
      }
    }
  }
  if (map_idx > MAX_PSYC_PAGES)
    cprintf("\nERROR - Too many Pages in Memory: %d", map_idx);
  else
  {
    cprintf("\nPage Mappings: ");
    for (int i = 0; i < map_idx; i++)
      cprintf("\n%d ----> %d", mappings[i][0], mappings[i][1]);
  }
}

void swapIn(uint faultingVa) {
  struct proc *p = myproc();

  if (p->memPageCount == MAX_PSYC_PAGES) {
    if( p->verbose>=1 ) cprintf("\n");
    swapOut(p);
  }

  faultingVa = PGROUNDDOWN(faultingVa);
  for (int i = 0; i < MAX_SWAP_PAGES; i++)
  {
    if (p->swapMap[i] == faultingVa)
    {

      if( p->verbose>=1 ) {
        cprintf("Swapping In Page: %d, PID: %d\n", faultingVa>>12, p->pid);
      }

      char *mem = kalloc();
      memset(mem, 0, PGSIZE);
      lcr3(V2P(p->pgdir));

      pte_t *pgtable_entry = walkpgdir(p->pgdir, (int *)faultingVa, 0);

      if (pgtable_entry == 0)
        panic("swapIn: PTE does not exsit in PGDIR");
      if (*pgtable_entry & PTE_P)
        panic("swapIn: Page Already in RAM");

      *pgtable_entry = V2P(mem) | PTE_P | PTE_U | PTE_W;
      *pgtable_entry &= ~PTE_PG;

      lcr3(V2P(p->pgdir));

      if (readFromSwapFile(p, (char *)mem, i * PGSIZE, PGSIZE) != PGSIZE)
      {
        panic("swapIn: Failed to Read from File");
      }

      p->swapMap[i] = 1;
      p->swapPageCount--;
      p->memPageCount++;
      p->pagesFIFO[p->memPageCount - 1] = faultingVa;
      p->pagesAge[p->memPageCount - 1] = 0x80000000;
      return;
    }
  }
}
