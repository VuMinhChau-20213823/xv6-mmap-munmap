// kernel/vma.c
// Quan ly VMA (vm_area_struct) cho xv6-riscv:
//  - danh sach lien ket VMA (sap xep theo dia chi) trong moi tien trinh
//  - tim khoang trong First-Fit / Best-Fit
//  - mmap / munmap (cat dau, cat duoi, tach doi, xoa han)
//  - lazy allocation khi page fault
//  - write-back trang dirty cua MAP_SHARED, sao chep VMA khi fork

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"
#include "vma.h"

pte_t *walk(pagetable_t, uint64, int);   // dinh nghia trong vm.c

int vma_policy = VMA_BEST_FIT;           // chien luoc hien tai (doi bang syscall vmapolicy)

// ---------------------------------------------------------------
// Tien ich
// ---------------------------------------------------------------

static uint64 maxu(uint64 a, uint64 b) { return a > b ? a : b; }
static uint64 minu(uint64 a, uint64 b) { return a < b ? a : b; }

static struct vm_area_struct *
vma_alloc(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(!p->vmas[i].vm_used){
      memset(&p->vmas[i], 0, sizeof(p->vmas[i]));
      p->vmas[i].vm_used = 1;
      return &p->vmas[i];
    }
  }
  return 0;
}

static void
vma_free_slot(struct vm_area_struct *v)
{
  memset(v, 0, sizeof(*v));              // vm_used = 0
}

static int
vma_has_free_slot(struct proc *p)
{
  for(int i = 0; i < NVMA; i++)
    if(!p->vmas[i].vm_used)
      return 1;
  return 0;
}

static struct vm_area_struct *
vma_find(struct proc *p, uint64 va)
{
  for(struct vm_area_struct *v = p->vma_head; v; v = v->vm_next)
    if(va >= v->vm_start && va < v->vm_end)
      return v;
  return 0;
}

static int
prot_to_pte(int prot)
{
  int f = PTE_U;
  if(prot & PROT_READ)  f |= PTE_R;
  if(prot & PROT_WRITE) f |= PTE_R | PTE_W;   // RISC-V: W can di kem R
  if(prot & PROT_EXEC)  f |= PTE_X;
  return f;
}

static int
vma_is_shared_rw(struct vm_area_struct *v)
{
  return v->vm_file && (v->vm_flags & MAP_SHARED) && (v->vm_prot & PROT_WRITE);
}

void
vma_init(struct proc *p)
{
  memset(p->vmas, 0, sizeof(p->vmas));
  p->vma_head = 0;
}

// ---------------------------------------------------------------
// Thuat toan cap phat: First-Fit / Best-Fit tren cac "lo trong"
// giua cac VMA lien tiep trong [MMAP_BASE, MMAP_TOP).
// Tra ve dia chi dau lo trong (0 neu khong co); *prev_out = VMA
// dung truoc lo trong do (0 neu lo trong nam o dau danh sach).
// ---------------------------------------------------------------
static uint64
vma_find_gap(struct proc *p, uint64 len, struct vm_area_struct **prev_out)
{
  struct vm_area_struct *v = p->vma_head;
  struct vm_area_struct *prev = 0, *best_prev = 0;
  uint64 cursor = MMAP_BASE;
  uint64 best_start = 0, best_size = 0;
  int found = 0;

  for(;;){
    uint64 gap_end = v ? v->vm_start : MMAP_TOP;
    if(gap_end >= cursor && gap_end - cursor >= len){
      uint64 size = gap_end - cursor;
      if(vma_policy == VMA_FIRST_FIT){          // First-Fit: lo dau tien du lon
        *prev_out = prev;
        return cursor;
      }
      if(!found || size < best_size){           // Best-Fit: lo nho nhat van du lon
        found = 1;
        best_size = size;
        best_start = cursor;
        best_prev = prev;
      }
    }
    if(v == 0)
      break;
    cursor = v->vm_end;
    prev = v;
    v = v->vm_next;
  }
  if(!found)
    return 0;
  *prev_out = best_prev;
  return best_start;
}

// ---------------------------------------------------------------
// mmap
// ---------------------------------------------------------------
uint64
vma_mmap(struct proc *p, uint64 addr, uint64 length, int prot, int flags,
         int fd, uint64 offset)
{
  struct file *f = 0;
  struct vm_area_struct *v, *prev = 0;
  uint64 start;

  (void)addr;                                   // addr chi la goi y, bo qua
  if(length == 0 || length > MMAP_TOP - MMAP_BASE)
    return -1;
  length = PGROUNDUP(length);
  if(prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
    return -1;
  if(((flags & MAP_SHARED) != 0) == ((flags & MAP_PRIVATE) != 0))
    return -1;                                  // phai co dung 1 trong 2

  if(flags & MAP_ANONYMOUS){
    offset = 0;
  } else {
    if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
      return -1;
    if(f->type != FD_INODE || !f->readable)
      return -1;
    if((flags & MAP_SHARED) && (prot & PROT_WRITE) && !f->writable)
      return -1;
    if(offset % PGSIZE)
      return -1;
  }

  v = vma_alloc(p);
  if(v == 0)
    return -1;
  start = vma_find_gap(p, length, &prev);
  if(start == 0){
    vma_free_slot(v);
    return -1;
  }

  v->vm_start = start;
  v->vm_end   = start + length;
  v->vm_prot  = prot;
  v->vm_flags = flags;
  v->vm_pgoff = offset;
  v->vm_file  = f ? filedup(f) : 0;

  if(prev){                                     // chen vao danh sach theo thu tu dia chi
    v->vm_next = prev->vm_next;
    prev->vm_next = v;
  } else {
    v->vm_next = p->vma_head;
    p->vma_head = v;
  }
  return start;
}

// ---------------------------------------------------------------
// Ghi mot trang cua MAP_SHARED ve file (cat theo kich thuoc file,
// chia nho de khong vuot qua gioi han giao dich cua log).
// ---------------------------------------------------------------
static void
vma_writeback_page(struct vm_area_struct *v, uint64 va, uint64 pa)
{
  struct inode *ip = v->vm_file->ip;
  uint64 off = v->vm_pgoff + (va - v->vm_start);
  int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
  uint64 size, total, done = 0;

  ilock(ip);
  size = ip->size;
  iunlock(ip);
  if(off >= size)
    return;
  total = minu(PGSIZE, size - off);

  while(done < total){
    uint64 n = minu(total - done, max);
    int r;
    begin_op();
    ilock(ip);
    r = writei(ip, 0, pa + done, off + done, n);
    iunlock(ip);
    end_op();
    if(r <= 0)
      break;
    done += r;
  }
}

// Ghi cac trang dirty trong [s, t) ve file. Neu clean != 0 thi go
// quyen ghi + bit dirty de lan ghi sau se bao fault lai.
static void
vma_flush_range(struct proc *p, struct vm_area_struct *v,
                uint64 s, uint64 t, int clean)
{
  if(!vma_is_shared_rw(v))
    return;
  for(uint64 a = s; a < t; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;
    if(*pte & VMA_PTE_DIRTY){
      vma_writeback_page(v, a, PTE2PA(*pte));
      if(clean){
        *pte &= ~(PTE_W | VMA_PTE_DIRTY);
        sfence_vma();
      }
    }
  }
}

// Giai phong cac trang vat ly dang map trong [s, t) (co write-back neu can).
static void
vma_unmap_pages(struct proc *p, struct vm_area_struct *v, uint64 s, uint64 t)
{
  vma_flush_range(p, v, s, t, 0);
  for(uint64 a = s; a < t; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;
    kfree((void*)PTE2PA(*pte));
    *pte = 0;
  }
  sfence_vma();
}

// ---------------------------------------------------------------
// munmap: xu ly 4 truong hop cho tung VMA giao voi [addr, addr+len)
//   1. phu tron VMA          -> xoa VMA
//   2. phu phan dau          -> cat dau  (vm_start tang, vm_pgoff tang)
//   3. phu phan cuoi         -> cat duoi (vm_end giam)
//   4. nam giua VMA          -> tach doi thanh 2 VMA
// ---------------------------------------------------------------
int
vma_munmap(struct proc *p, uint64 addr, uint64 length)
{
  struct vm_area_struct *v, *prev, *next, *nv;
  uint64 end;

  if(addr % PGSIZE || length == 0)
    return -1;
  length = PGROUNDUP(length);
  end = addr + length;
  if(end < addr || addr < MMAP_BASE || end > MMAP_TOP)
    return -1;

  // Truong hop tach doi can them 1 slot: kiem tra truoc khi sua doi bat ky thu gi.
  for(v = p->vma_head; v; v = v->vm_next){
    if(v->vm_start < addr && end < v->vm_end){
      if(!vma_has_free_slot(p))
        return -1;
      break;
    }
  }

  prev = 0;
  v = p->vma_head;
  while(v){
    next = v->vm_next;
    if(v->vm_start >= end)
      break;
    uint64 s = maxu(v->vm_start, addr);
    uint64 t = minu(v->vm_end, end);
    if(s >= t){                                  // khong giao nhau
      prev = v;
      v = next;
      continue;
    }

    vma_unmap_pages(p, v, s, t);

    if(s == v->vm_start && t == v->vm_end){      // 1. xoa han
      if(prev) prev->vm_next = next; else p->vma_head = next;
      if(v->vm_file)
        fileclose(v->vm_file);
      vma_free_slot(v);
      v = next;                                  // prev giu nguyen
      continue;
    } else if(s == v->vm_start){                 // 2. cat dau
      if(v->vm_file)
        v->vm_pgoff += t - s;
      v->vm_start = t;
    } else if(t == v->vm_end){                   // 3. cat duoi
      v->vm_end = s;
    } else {                                     // 4. tach doi
      nv = vma_alloc(p);                         // da kiem tra con slot
      nv->vm_start = t;
      nv->vm_end   = v->vm_end;
      nv->vm_prot  = v->vm_prot;
      nv->vm_flags = v->vm_flags;
      nv->vm_file  = v->vm_file ? filedup(v->vm_file) : 0;
      nv->vm_pgoff = v->vm_pgoff + (t - v->vm_start);
      nv->vm_next  = next;
      v->vm_end    = s;
      v->vm_next   = nv;
    }
    prev = v;
    v = next;
  }
  return 0;
}

// Go bo toan bo VMA cua tien trinh (exit / exec / loi fork).
void
vma_unmap_all(struct proc *p)
{
  vma_munmap(p, MMAP_BASE, MMAP_TOP - MMAP_BASE);
}

// ---------------------------------------------------------------
// Lazy allocation: duoc goi tu usertrap (scause 12/13/15) va tu
// walkaddr (scause = 0, nghia la doc).
// Tra ve 0 neu da xu ly xong, -1 neu la truy cap khong hop le.
// ---------------------------------------------------------------
int
vma_fault(struct proc *p, uint64 va, uint64 scause)
{
  struct vm_area_struct *v;
  pte_t *pte;
  uint64 a;
  char *mem;
  int need, is_write = 0;

  if(va >= MAXVA)
    return -1;
  v = vma_find(p, va);
  if(v == 0)
    return -1;

  if(scause == 12)      need = PROT_EXEC;
  else if(scause == 15){ need = PROT_WRITE; is_write = 1; }
  else                  need = PROT_READ;
  if(!(v->vm_prot & need))
    return -1;

  a = PGROUNDDOWN(va);
  pte = walk(p->pagetable, a, 0);

  if(pte && (*pte & PTE_V)){
    // Trang da co mat: chi hop le khi la lan ghi dau tien vao trang
    // MAP_SHARED (da map read-only de theo doi dirty).
    if(is_write && vma_is_shared_rw(v) && !(*pte & PTE_W)){
      *pte |= PTE_W | VMA_PTE_DIRTY;
      sfence_vma();
      return 0;
    }
    return -1;
  }

  mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);

  if(v->vm_file){
    struct inode *ip = v->vm_file->ip;
    uint64 off = v->vm_pgoff + (a - v->vm_start);
    ilock(ip);
    readi(ip, 0, (uint64)mem, off, PGSIZE);     // phan vuot qua EOF van la 0
    iunlock(ip);
  }

  int perm = prot_to_pte(v->vm_prot);
  if(vma_is_shared_rw(v)){
    if(is_write)
      perm |= VMA_PTE_DIRTY;                    // ghi ngay: danh dau dirty
    else
      perm &= ~PTE_W;                           // doc truoc: map read-only
  }
  if(mappages(p->pagetable, a, PGSIZE, (uint64)mem, perm) != 0){
    kfree(mem);
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------
// fork: sao chep danh sach VMA (giu nguyen thu tu dia chi).
//  - MAP_PRIVATE / anonymous: sao chep cac trang da co mat
//  - MAP_SHARED co file: ghi cac trang dirty cua cha ve file, con se
//    lazy-fault doc lai tu file (chua chia se trang vat ly)
// Phai goi KHI KHONG giu spinlock (co the ngu khi truy cap file).
// ---------------------------------------------------------------
int
vma_fork(struct proc *p, struct proc *np)
{
  struct vm_area_struct *pv, *nv, *tail = 0;

  vma_init(np);
  for(pv = p->vma_head; pv; pv = pv->vm_next){
    nv = vma_alloc(np);
    if(nv == 0)
      goto bad;
    nv->vm_start = pv->vm_start;
    nv->vm_end   = pv->vm_end;
    nv->vm_prot  = pv->vm_prot;
    nv->vm_flags = pv->vm_flags;
    nv->vm_pgoff = pv->vm_pgoff;
    nv->vm_file  = pv->vm_file ? filedup(pv->vm_file) : 0;
    nv->vm_next  = 0;
    if(tail) tail->vm_next = nv; else np->vma_head = nv;
    tail = nv;

    if(pv->vm_file && (pv->vm_flags & MAP_SHARED)){
      vma_flush_range(p, pv, pv->vm_start, pv->vm_end, 1);
      continue;
    }
    for(uint64 a = pv->vm_start; a < pv->vm_end; a += PGSIZE){
      pte_t *pte = walk(p->pagetable, a, 0);
      char *mem;
      if(pte == 0 || (*pte & PTE_V) == 0)
        continue;
      mem = kalloc();
      if(mem == 0)
        goto bad;
      memmove(mem, (char*)PTE2PA(*pte), PGSIZE);
      if(mappages(np->pagetable, a, PGSIZE, (uint64)mem,
                  *pte & (PTE_R | PTE_W | PTE_X | PTE_U)) != 0){
        kfree(mem);
        goto bad;
      }
    }
  }
  return 0;

bad:
  vma_unmap_all(np);
  return -1;
}
