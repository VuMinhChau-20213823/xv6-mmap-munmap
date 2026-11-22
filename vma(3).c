// kernel/vma.c
// Quan ly VMA (vm_area_struct) cho xv6-riscv:
//  - danh sach lien ket VMA (sap xep theo dia chi) trong moi tien trinh
//  - tim khoang trong First-Fit / Best-Fit (hoac dung goi y addr neu con trong)
//  - mmap / munmap (cat dau, cat duoi, tach doi, xoa han)
//  - gop cac VMA ke nhau co cung thuoc tinh (giam so VMA, chong phan manh)
//  - lazy allocation khi page fault
//  - write-back trang dirty cua MAP_SHARED, sao chep VMA khi fork
//  - fork chia se trang vat ly cua vung MAP_SHARED (cha va con cung tro vao
//    mot dia chi vat ly) + bo dem tham chieu (reference count) cho trang
//  - kiem tra tinh toan ven cua danh sach (VMA_DEBUG)

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
// Bo dem tham chieu (reference count) cho cac trang vat ly cua vung mmap.
// pgref[i] = so tien trinh dang map trang vat ly thu i.
//  - trang vua cap phat de nap vao mot VMA: ref = 1
//  - fork chia se them 1 tien trinh: ref++
//  - munmap / exit: ref--, chi khi ref == 0 moi tra trang ve kalloc (kfree)
// Dung phep nguyen tu cua GCC nen khong can khoa.
// ---------------------------------------------------------------
#define PGREF_N ((PHYSTOP - KERNBASE) / PGSIZE)
static int pgref[PGREF_N];

static int
pgref_idx(uint64 pa)
{
  if(pa % PGSIZE || pa < KERNBASE || pa >= PHYSTOP)
    panic("pgref_idx");
  return (pa - KERNBASE) / PGSIZE;
}

static void
pgref_init(void *mem)                    // trang moi: ref = 1
{
  int i = pgref_idx((uint64)mem);
  if(pgref[i] != 0)
    panic("pgref_init: page already referenced");
  pgref[i] = 1;
}

static void
pgref_inc(uint64 pa)                     // them mot tien trinh cung dung trang
{
  if(__sync_add_and_fetch(&pgref[pgref_idx(pa)], 1) < 2)
    panic("pgref_inc");
}

// Tra lai 1 tham chieu; giai phong trang vat ly khi khong con ai dung.
static void
vma_put_page(uint64 pa)
{
  int n = __sync_sub_and_fetch(&pgref[pgref_idx(pa)], 1);
  if(n < 0)
    panic("vma_put_page: refcount < 0");
  if(n == 0)
    kfree((void*)pa);
}

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

// Quan sat bo dem tham chieu (dung cho syscall vmainfo, test va debug):
// tra ve so tien trinh dang map trang vat ly ung voi dia chi ao va cua p,
// hoac -1 neu trang do chua co mat (khong gay page fault).
int
vma_pgref_of(struct proc *p, uint64 va)
{
  pte_t *pte;

  if(va >= MAXVA)
    return -1;
  pte = walk(p->pagetable, PGROUNDDOWN(va), 0);
  if(pte == 0 || (*pte & PTE_V) == 0)
    return -1;
  return pgref[pgref_idx(PTE2PA(*pte))];
}

// ---------------------------------------------------------------
// Kiem tra tinh toan ven cua danh sach VMA (chi chay khi VMA_DEBUG != 0):
//  - moi nut dang dung, can theo trang, vm_start < vm_end
//  - sap xep tang dan, khong chong lan, nam trong [MMAP_BASE, MMAP_TOP)
//  - khong co vong lap; so nut trong danh sach = so slot dang dung
// ---------------------------------------------------------------
static void
vma_check(struct proc *p)
{
#if VMA_DEBUG
  int used = 0, listed = 0;
  uint64 last_end = MMAP_BASE;

  for(int i = 0; i < NVMA; i++)
    if(p->vmas[i].vm_used)
      used++;

  for(struct vm_area_struct *v = p->vma_head; v; v = v->vm_next){
    if(++listed > NVMA)
      panic("vma_check: cycle");
    if(!v->vm_used)
      panic("vma_check: unused node in list");
    if(v->vm_start % PGSIZE || v->vm_end % PGSIZE)
      panic("vma_check: not page aligned");
    if(v->vm_start >= v->vm_end)
      panic("vma_check: empty or negative vma");
    if(v->vm_start < last_end)
      panic("vma_check: overlap or bad order");
    if(v->vm_end > MMAP_TOP)
      panic("vma_check: beyond MMAP_TOP");
    last_end = v->vm_end;
  }
  if(listed != used)
    panic("vma_check: slot count mismatch");
#else
  (void)p;
#endif
}

// ---------------------------------------------------------------
// Gop VMA: hai VMA ke nhau (a ngay truoc b) gop duoc khi lien tiep ve dia chi
// va giong het ve prot, flags, file (va offset trong file lien tiep).
// ---------------------------------------------------------------
static int
vma_can_merge(struct vm_area_struct *a, struct vm_area_struct *b)
{
  if(a->vm_end != b->vm_start)
    return 0;
  if(a->vm_prot != b->vm_prot || a->vm_flags != b->vm_flags)
    return 0;
  if(a->vm_file != b->vm_file)
    return 0;
  if(a->vm_file && a->vm_pgoff + VMA_LEN(a) != b->vm_pgoff)
    return 0;
  return 1;
}

// Gop a voi VMA ke sau no neu duoc. Tra ve 1 neu da gop (b bi xoa).
static int
vma_merge_next(struct vm_area_struct *a)
{
  struct vm_area_struct *b = a->vm_next;

  if(b == 0 || !vma_can_merge(a, b))
    return 0;
  a->vm_end = b->vm_end;
  a->vm_next = b->vm_next;
  if(b->vm_file)
    fileclose(b->vm_file);               // b giu 1 tham chieu rieng; a van con tham chieu cua no
  vma_free_slot(b);
  return 1;
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

// Goi y addr: dung duoc neu can theo trang, nam trong [MMAP_BASE, MMAP_TOP)
// va [addr, addr+len) khong cat VMA nao. *prev_out = VMA dung truoc vung do.
static int
vma_hint_ok(struct proc *p, uint64 addr, uint64 len, struct vm_area_struct **prev_out)
{
  struct vm_area_struct *v, *prev = 0;
  uint64 end = addr + len;

  if(addr == 0 || addr % PGSIZE || end < addr || addr < MMAP_BASE || end > MMAP_TOP)
    return 0;
  for(v = p->vma_head; v; v = v->vm_next){
    if(v->vm_end <= addr){
      prev = v;
      continue;
    }
    if(v->vm_start >= end)
      break;
    return 0;                                   // chong lan voi VMA co san
  }
  *prev_out = prev;
  return 1;
}

// ---------------------------------------------------------------
// mmap
// ---------------------------------------------------------------
uint64
vma_mmap(struct proc *p, uint64 addr, uint64 length, int prot, int flags,
         int fd, uint64 offset)
{
  struct file *f = 0;
  struct vm_area_struct *v, *cur, *prev = 0;
  uint64 start;

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
    if(offset % PGSIZE || offset + length < offset)
      return -1;
  }

  v = vma_alloc(p);
  if(v == 0)
    return -1;

  // addr la goi y: dung neu vung do con trong, nguoc lai chon theo chinh sach
  if(addr != 0 && vma_hint_ok(p, addr, length, &prev))
    start = addr;
  else
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

  // Gop voi VMA lien ke (truoc va/hoac sau) neu cung thuoc tinh
  cur = v;
  if(prev && vma_merge_next(prev))
    cur = prev;
  vma_merge_next(cur);

  vma_check(p);
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

// Go cac trang dang map trong [s, t): write-back neu can, roi tra tham chieu.
static void
vma_unmap_pages(struct proc *p, struct vm_area_struct *v, uint64 s, uint64 t)
{
  vma_flush_range(p, v, s, t, 0);
  for(uint64 a = s; a < t; a += PGSIZE){
    pte_t *pte = walk(p->pagetable, a, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;
    vma_put_page(PTE2PA(*pte));          // tra 1 tham chieu (chi free khi ref == 0)
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
  if(length > MMAP_TOP - MMAP_BASE)
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

  vma_check(p);
  return 0;
}

// Go bo toan bo VMA cua tien trinh (exit / exec / loi fork).
void
vma_unmap_all(struct proc *p)
{
  vma_munmap(p, MMAP_BASE, MMAP_TOP - MMAP_BASE);
}

// Cap phat 1 trang vat ly, nap noi dung (0 hoac tu file), map vao tien trinh p
// tai dia chi a (can theo trang) va dat ref = 1. is_write: lan truy cap dau la ghi.
static int
vma_populate(struct proc *p, struct vm_area_struct *v, uint64 a, int is_write)
{
  char *mem;
  int perm;

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

  perm = prot_to_pte(v->vm_prot);
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
  pgref_init(mem);
  return 0;
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

  return vma_populate(p, v, a, is_write);
}

// ---------------------------------------------------------------
// fork: sao chep danh sach VMA (giu nguyen thu tu dia chi).
//  - MAP_SHARED (VMA_SHARE_PAGES = 1): cha va con CUNG TRO VAO MOT TRANG VAT LY.
//      + trang chua co mat o cha duoc nap truoc (anonymous: trang 0; file: doc
//        tu file) de moi trang cua vung deu duoc chia se ngay tu luc fork;
//      + moi trang vat ly duoc tang bo dem tham chieu (pgref_inc);
//      + vung co file va ghi duoc: con map read-only va co bit dirty rieng,
//        de ghi cua con duoc ghi ve file khi con munmap / exit.
//  - MAP_PRIVATE: van sao chep rieng tung trang da co mat.
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

    if(pv->vm_prot == 0)                        // PROT_NONE: khong co gi de chia se / sao chep
      continue;

    if(VMA_SHARE_PAGES && (pv->vm_flags & MAP_SHARED)){
      for(uint64 a = pv->vm_start; a < pv->vm_end; a += PGSIZE){
        pte_t *pte = walk(p->pagetable, a, 0);
        uint64 pa;
        int perm;
        if(pte == 0 || (*pte & PTE_V) == 0){
          if(vma_populate(p, pv, a, 0) < 0)     // nap truoc o cha de co trang chung
            goto bad;
          pte = walk(p->pagetable, a, 0);
        }
        pa = PTE2PA(*pte);
        perm = *pte & (PTE_R | PTE_W | PTE_X | PTE_U);
        if(vma_is_shared_rw(pv))
          perm &= ~PTE_W;                       // con tu theo doi dirty cua minh
        if(mappages(np->pagetable, a, PGSIZE, pa, perm) != 0)
          goto bad;
        pgref_inc(pa);                          // them 1 tien trinh dung trang nay
      }
      continue;
    }

    if(pv->vm_file && (pv->vm_flags & MAP_SHARED)){   // chi khi VMA_SHARE_PAGES = 0
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
      pgref_init(mem);
    }
  }
  vma_check(np);
  return 0;

bad:
  vma_unmap_all(np);
  return -1;
}
