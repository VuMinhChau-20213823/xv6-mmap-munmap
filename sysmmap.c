// kernel/sysmmap.c
// Khung thuc thi system call: sys_mmap, sys_munmap, sys_vmapolicy.
// Logic that nam trong vma.c; o day chi lay tham so tu trapframe.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"

extern int vma_policy;

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
