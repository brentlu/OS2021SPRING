#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "file.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

/*
 * vmarea pool
 */
struct vmarea vma_cache[NVMAREA];
struct vmarea *vma_head = 0;

/*
 * local static funcion definition
 */
static void _mfree(struct proc *, struct vmarea *, uint64, uint64, int);

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // map kernel stacks
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();

  for(int i = 0; i < NVMAREA; i++){
    vma_cache[i].next = vma_head;
    vma_head = &vma_cache[i];
  }
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
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
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
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int perm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  if(!perm)
    perm = PTE_W|PTE_X|PTE_R|PTE_U;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, perm) != 0){
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
uvmcopy(pagetable_t old, pagetable_t new, uint64 start, uint64 end, int sparse)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  if(start % PGSIZE)
    panic("uvmcopy: not aligned");

  for(i = start; i < end; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0){
      if(!sparse)
        panic("uvmcopy: pte should exist");
      else
        continue;
    }
    if((*pte & PTE_V) == 0){
      if(!sparse)
        panic("uvmcopy: page not present");
      else
        continue;
    }
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
  uvmunmap(new, start, i / PGSIZE, 1);
  return -1;
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

static void
_vmprint(pte_t *pd, int level)
{
  for(int i = 0; i < PGSIZE / sizeof(pte_t); i++){
    if((pd[i] & PTE_V) == 0)
      continue;

    for(int j = 0; j < level; j++)
      printf(" ..");

    printf("%d: pte %p pa %p\n", i, pd[i], PTE2PA(pd[i]));

    if(level != 3)
      _vmprint((pte_t *)PTE2PA(pd[i]), level + 1);
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);

  _vmprint(pagetable, 1);
}

uint64
mmap(uint64 addr, uint length, int prot, int flags, struct file *f, int offset)
{
  struct proc *p = myproc();
  struct vmarea *a, *b, *c;

  int first = 0;

  if(offset % PGSIZE)
    panic("mmap: not aligned");

  addr = PGROUNDUP(addr);
  length = PGROUNDUP(length);

  if(addr < MMAP || addr + length > TRAPFRAME){
    first = 1; /* invalid user request, try first fit */
    addr = MMAP;
  }

_again:
  if(!p->mmap || addr + length <= p->mmap->start){
    /* space found before first vmarea */
    a = 0;
    b = p->mmap;
  } else {
    /* search hole from first vmarea */
    for(a = p->mmap, b = 0; a != 0; a = a->next){
      b = a->next;

      if(!first){
        /* check user request address */
        if(a->end <= addr){
          if(!b || addr + length <= b->start)
            break; /* found */
        } else {
          first = 1; /* fail, try first fit next */
          addr = MMAP;
          goto _again;
        }
      } else {
        if(!b){
          if (a->end + length <= TRAPFRAME){
            addr = a->end;
            break; /* found */
          } else
            return -1; /* out of address space */
        }

        if(a->end + length <= b->start){
          addr = a->end;
          break; /* found */
        }
      }
    }
  }

  c = vma_head;
  if(!c)
    return -1;

  if(vma_head)
    vma_head = vma_head->next;

  /* a->c->b */
  if(a)
    a->next = c;
  else
    p->mmap = c; /* c is the head */

  c->start = addr;
  c->end = c->start + length;
  c->next = b;

  c->page_prot = PTE_U;
  if(prot & PROT_READ)
    c->page_prot |= PTE_R;
  if(prot & PROT_WRITE)
    c->page_prot |= PTE_W;
  if(prot & PROT_EXEC)
    c->page_prot |= PTE_X;

  c->flags = flags;
  c->pgoff = offset;
  if(f)
    c->file = filedup(f); /* increase reference count */
  else
    c->file = 0;

  return c->start;
}

uint64
munmap(uint64 addr, uint length)
{
  struct proc *p = myproc();
  struct vmarea *a, *b, *c;
  uint64 eddr = addr + length;

  if(addr % PGSIZE)
    panic("munmap: not aligned");

  length = PGROUNDUP(length);

  for(a = p->mmap, b = 0; a != 0; a = a->next){
    if(eddr <= a->start || addr >= a->end) /* no overlap */
      continue;
    else if(addr <= a->start && eddr >= a->end){
      /* overlap: entire area */
      _mfree(p, a, a->start, a->end, 1);

      /* release the file */
      if(a->file){
        fileclose(a->file);
        a->file = 0;
      }

      /* remove this vmarea from process's mmap list */
      if (!b)
        p->mmap = a->next;
      else
        b->next = a->next;

      /* insert to free list */
      a->next = vma_head;
      vma_head = a;
    } else if(addr > a->start && eddr < a->end){
      /* overlap: middle area */
      _mfree(p, a, addr, eddr, 1);

      /* allocate new vmarea */
      c = vma_head;
      if(!c)
        return -1;

      if(vma_head)
        vma_head = vma_head->next;

      c->start = eddr;
      c->end = a->end;
      c->page_prot = a->page_prot;
      c->flags = a->flags;
      c->pgoff = a->pgoff + eddr - a->start;
      if(a->file)
        c->file = filedup(a->file);
      else
        c->file = 0;

      /* update end address */
      a->end = addr;

      /* a->c */
      c->next = a->next;
      a->next = c;
    } else if(addr <= a->start && eddr < a->end){
      /* overlap: front area */
      _mfree(p, a, a->start, eddr, 1);

      /* update file offset and start address */
      a->pgoff += eddr - a->start;
      a->start = eddr;
    } else {
      /* overlap: tail area */
      _mfree(p, a, addr, a->end, 1);

      /* update end address */
      a->end = addr;
    }

    b = a;
  }

  return 0;
}

// Close all mmap files
// This function is called when process exits.
void
mclose(void)
{
  struct proc *p = myproc();
  struct vmarea *a;

  for(a = p->mmap; a != 0; a = a->next){
    _mfree(p, a, a->start, a->end, 0);

    /* release the file */
    if(a->file){
      fileclose(a->file);
      a->file = 0;
    }
  }
}

// Free all physical pages allocated to mmap regions
// This function is called in parent process.
void
mfree(struct proc *p)
{
  struct vmarea *a, *b;

  for(a = p->mmap, b = 0; a != 0; a = a->next){
    _mfree(p, a, a->start, a->end, 1);

    b = a;
  }

  if(b){
    /* insert to free list */
    b->next = vma_head;
    vma_head = p->mmap;
  }
}

// Copy all vm areas from parent to child
// New physical memory will be allocated regardless vm flag in this
// implementation.
int
mcopy(struct proc *np)
{
  struct proc *op = myproc();
  struct vmarea *a, *b, *c;

  for(a = op->mmap, b = 0; a != 0; a = a->next){
    c = vma_head;
    if(!c)
      goto _fail;

    if(vma_head)
      vma_head = vma_head->next;

    /* allocate new vmarea */
    c->start = a->start;
    c->end = a->end;
    c->page_prot = a->page_prot;
    c->flags = a->flags;
    c->pgoff = a->pgoff;
    if(a->file)
      c->file = filedup(a->file);
    else
      c->file = 0;

    /* allocate new physical pages */
    if(uvmcopy(op->pagetable, np->pagetable, c->start, c->end, 1))
      goto _fail;

    if(!b)
      np->mmap = c;
    else
      b->next = c;

    b = c;
  }

  if(b)
    b->next = 0;

  return 0;

_fail:
  /* complete the link list then return error */
  if(!b)
    np->mmap = c;
  else
    b->next = c;

  if(c)
    c->next = 0;

  return -1;
}

int
mtrap(uint64 addr)
{
  struct proc *p = myproc();
  struct vmarea *a;
  int r = 0;

  addr = PGROUNDDOWN(addr);

  for(a = p->mmap; a != 0; a = a->next){
    if(addr < a->start || addr >= a->end) /* not this area */
      continue;

    /* a new physical page dedicated to this process */
    if(!uvmalloc(p->pagetable, addr, addr + PGSIZE, a->page_prot))
      return -1;

    /* update page from file if possible */
    if(a->file && a->file->readable){
      filelseek(a->file, a->pgoff + addr - a->start, SEEK_SET);
      r = fileread(a->file, addr, PGSIZE);
      if(r < 0)
        return -1;
    }

    return 0;
  }

  return -1;
}

static void
_mfree(struct proc *p, struct vmarea *a, uint64 start, uint64 end, int do_free)
{
  uint64 addr;
  pte_t *pte;
  int offset;

  if(start % PGSIZE || end % PGSIZE)
    panic("_mfree: not aligned");

  offset = a->pgoff + start - a->start; /* file offset to read this page */

  for(addr = start; addr < end; addr += PGSIZE){
    if((pte = walk(p->pagetable, addr, 0)) == 0)
      panic("_mfree: walk");
    if((*pte & PTE_V) == 0) /* not valid page */
      continue;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("_mfree: not a leaf");

    if(a->flags & MAP_SHARED)
      if(a->file && a->file->writable && *pte & PTE_D){
        /* only if page is dirty */
        filelseek(a->file, offset + addr - start, SEEK_SET);
        filewrite(a->file, addr, PGSIZE);

        *pte &= ~PTE_D;
      }

    if(do_free){
      kfree((void *)PTE2PA(*pte));
      *pte = 0;
    }
  }
}
