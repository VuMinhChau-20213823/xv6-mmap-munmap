// kernel/sysmmap.c
// Khung thuc thi system call: sys_mmap, sys_munmap, sys_vmapolicy, sys_vmainfo.
// Logic that nam trong vma.c; o day chi lay tham so tu trapframe.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

extern int vma_policy;
extern uint64 kfreepages(void);               // kalloc.c: so trang vat ly dang tu do
int vma_pgref_of(struct proc*, uint64);       // vma.c: bo dem tham chieu cua mot trang

// void *mmap(void *addr, uint64 length, int prot, int flags, int fd, uint64 offset)
// Tra ve dia chi vung map, hoac (uint64)-1 neu loi.
uint64
sys_mmap(void)
{
  uint64 addr, length, offset;
  int prot, flags, fd;

  argaddr(0, &addr);
  argaddr(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &offset);

  return vma_mmap(myproc(), addr, length, prot, flags, fd, offset);
}

// int munmap(void *addr, uint64 length)
// Tra ve 0 neu thanh cong, -1 neu loi.
uint64
sys_munmap(void)
{
  uint64 addr, length;

  argaddr(0, &addr);
  argaddr(1, &length);

  return vma_munmap(myproc(), addr, length);
}

// int vmapolicy(int policy): 0 = First-Fit, 1 = Best-Fit. Tra ve chinh sach cu.
uint64
sys_vmapolicy(void)
{
  int policy, old;

  argint(0, &policy);
  if(policy != 0 && policy != 1)
    return -1;
  old = vma_policy;
  vma_policy = policy;
  return old;
}

// int vmainfo(int what, uint64 arg): quan sat bo dem tham chieu.
//   what = 0: bo dem tham chieu cua trang vat ly dang map tai dia chi arg
//             (-1 neu trang do chua co mat)
//   what = 1: so trang vat ly dang tu do trong he thong
//   what = 2: bo dem tham chieu (struct file ref) cua file descriptor arg
//             (-1 neu fd khong hop le)
uint64
sys_vmainfo(void)
{
  int what;
  uint64 arg;
  struct proc *p = myproc();

  argint(0, &what);
  argaddr(1, &arg);

  if(what == 0)
    return vma_pgref_of(p, arg);
  if(what == 1)
    return kfreepages();
  if(what == 2){
    if(arg >= NOFILE || p->ofile[arg] == 0)
      return -1;
    return p->ofile[arg]->ref;
  }
  return -1;
}
