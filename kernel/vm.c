#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

extern uchar cow_ref[];
extern struct spinlock cow_lock;

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];

    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      if(DEBUG) printf("freewalk: %d\n", i);
      if(DEBUG) printf("freewalk: pte %p\n", pte);
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy_original(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}
//PTE_U to PTE_U
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  if(DEBUG) printf("head of uvmcopy: sz %p\n",sz );
  if(DEBUG) uvmshow(old, sz);
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    if((*pte & PTE_U) == 0){
      if(DEBUG) printf("uvmcopy: not PTE_U va %p\n", i);
      //panic("uvmcopy: not PTE_U");
    }
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if(flags&PTE_U){
      //old is cow or not?
      if (flags&(PTE_R|PTE_COW_R)){
        flags |= PTE_COW_R;
        *pte |= PTE_COW_R;
      }
      if (flags&(PTE_W|PTE_COW_W)){
        flags &= ~PTE_W; //mark as not writable!
        flags |= PTE_COW_W;
        *pte |= PTE_COW_W;
        *pte &= ~PTE_W;
      }
    }
    else{
      ;//flags stay the same, ~PTE_U
    }
    if(DEBUG) printf("uvmcopy: acquire cowlock\n");
    acquire(&cow_lock);
    //if count is 0, add 2; else add 1
    if (cow_ref[COWREFINDEX(pa)] == 0) cow_ref[COWREFINDEX(pa)]++;
    cow_ref[COWREFINDEX(pa)]++;
    release(&cow_lock);
    if(mappages(new, i, PGSIZE, pa, flags) != 0){
      goto err;
    }
  }
  if(DEBUG) printf("return of uvmcopy\n");
  if(DEBUG) uvmshow(new, sz);
  return 0;

 err:
  panic("uvmcopy_cow: mappages fail");
}

//uncow a virtual page, optionally alloc new phy page
int
uvmuncow(pagetable_t pagetable, uint64 va)
{
  struct proc *p = myproc();
  pte_t *pte;
  uint64 pa;
  uint flags;
  if(DEBUG) printf("head of uvmuncow:va %p\n", va);
  if(DEBUG) uvmshow(pagetable, p->sz);

  if(va >= p->sz){
    if(DEBUG) printf("uvmuncow: va >= p->sz\n");
    return -1;
  }
  pte = walk(pagetable, va, 0);
  if(pte == 0){
    if(DEBUG) printf("uvmuncow: va not mapped in pagetable\n");
    return -1;
  }

  pa = PTE2PA(*pte);

  if(DEBUG) printf("uvmuncow: acquire cowlock\n");
  acquire(&cow_lock);
  if(cow_ref[COWREFINDEX(pa)]==0){
    panic("uvmuncow: pa not a cow page\n");
    //return -1;
  }
  release(&cow_lock);
  //stack guard page is pte not PTE_COW_R/W
  /*
  if( (*pte & (PTE_COW_R|PTE_COW_W)) == 0){
    printf("pid %d pte %p\nref count %d\n",p->pid, *pte, cow_ref[COWREFINDEX(pa)]);
    panic("uvmuncow: pte not PTE_COW_R/W");
  }
  */

  if(*pte & PTE_U){
    flags = PTE_V | PTE_X | PTE_U;
  }else{
    //if(va<p->sz) return -1;//userspace not PTE_U, kill
    if(DEBUG) printf("uvmuncow on Kernel page or guard page\n");
    if(!(*pte & (PTE_COW_R|PTE_COW_W))){
      if(DEBUG) printf("uvmuncow on not PTE_U and not PTE_COW_R|PTE_COW_W\n");
      return -1;
    }
    flags = PTE_V | PTE_X | PTE_U;
    //panic("uvmuncow:don't support k to u uncow");
  }

  if(*pte & PTE_COW_R) flags |= PTE_R;
  if(*pte & PTE_COW_W) flags |= PTE_W;

  if(DEBUG) printf("uvmuncow: acquire cowlock\n");
  acquire(&cow_lock);
  if (cow_ref[COWREFINDEX(pa)]>=2){
    if(DEBUG) printf("uvmuncow: alloc flags %p\n",flags);
    char *mem = kalloc();
    //uvmunmap; alloc new page
    if(mem == 0) {
      release(&cow_lock);
      return -1;
    }
    cow_ref[COWREFINDEX(pa)]--;

    uvmunmap(pagetable, PGROUNDDOWN(va), 1, 0);
    //memcopy !!!!!!
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(pagetable, PGROUNDDOWN(va), PGSIZE, (uint64)mem, flags) != 0){
      release(&cow_lock);
      return -1;//need PGROUNDDOWN for alignment, or will map one more
    }
  }
  else if(cow_ref[COWREFINDEX(pa)]==1){
    if(DEBUG) printf("uvmuncow: cow to not cow\n");

    //make not cow
    cow_ref[COWREFINDEX(pa)] = 0;

    *pte |= flags;
    *pte &= ~(PTE_COW_R|PTE_COW_W);
  }
  else{
    panic("uvmuncow: cow ref count 0");
  }
  release(&cow_lock);
  if(DEBUG) printf("return of uvmuncow\n");
  if(DEBUG) uvmshow(pagetable, p->sz);
  return 0;
}

void uvmshow(pagetable_t pagetable, uint64 sz)
{
  uint64 i;
  pte_t *pte;
  uint64 pa;
  struct proc *p = myproc();
  printf("uvmshow pid %d sz %p\n", p->pid, p->sz);
  for (i=0;i<sz;i+=0x1000) {
    pte = walk(pagetable, i, 0);
    if(pte == 0){
      printf("  %p 0\n",i);
      continue;
    }
    pa = PTE2PA(*pte);
    printf("  %p %p ",i,pa);
    if (*pte & PTE_COW_W) printf("COW_W ");
    else printf("XXXXX ");

    if (*pte & PTE_COW_R) printf("COW_R ");
    else {printf("XXXXX ");}

    if (*pte & PTE_U) printf("PTE_U ");
    else {printf("XXXXX ");}

    if (*pte & PTE_X) printf("PTE_X ");
    else {printf("XXXXX ");}

    if (*pte & PTE_W) printf("PTE_W ");
    else {printf("XXXXX ");}

    if (*pte & PTE_R) printf("PTE_R ");
    else {printf("XXXXX ");}

    if (*pte & PTE_V) printf("PTE_V ");
    else {printf("XXXXX ");}

    printf("%d\n",cow_ref[COWREFINDEX(pa)]);
  }
  return;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}
int
copyout_cow(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  if(DEBUG) printf("head of copyout: len %p\n",len );
  struct proc *p = myproc();
  if(DEBUG) uvmshow(pagetable, p->sz);
  for(i = PGROUNDDOWN((uint64) src); i < (uint64) src + len; i+=PGSIZE, dstva+=PGSIZE){
    if((pte = walk(kernel_pagetable, i, 0)) == 0)
      panic("copyout: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("copyout: page not present");

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if(flags){
      if (flags&(PTE_R|PTE_COW_R)){
        flags |= PTE_COW_R;
        *pte |= PTE_COW_R;
      }
      if (flags&(PTE_W|PTE_COW_W)){
        flags &= ~PTE_W; //mark as not writable!
        flags |= PTE_COW_W;
        *pte |= PTE_COW_W;
        *pte &= ~PTE_W;
      }
    }

    if(DEBUG) printf("copyout: acquire cowlock\n");
    acquire(&cow_lock);
    //if count is 0, add 2; else add 1
    if (cow_ref[COWREFINDEX(pa)] == 0) cow_ref[COWREFINDEX(pa)]++;
    cow_ref[COWREFINDEX(pa)]++;
    release(&cow_lock);
    if(mappages(pagetable, dstva, PGSIZE, pa, flags) != 0){
      goto err;
    }
  }
  if(DEBUG) printf("return of copyout\n");
  if(DEBUG) uvmshow(pagetable, p->sz);
  return 0;

 err:
  panic("uvmcopy_cow: mappages fail");
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
