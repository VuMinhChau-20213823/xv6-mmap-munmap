// kernel/vma.h
// Cau truc vung nho ao (VMA) cho xv6-riscv.
//
// Moi tien trinh co mot bang NVMA phan tu (p->vmas) va mot danh sach lien ket
// don (p->vma_head -> vm_next) noi cac VMA dang dung, SAP XEP TANG DAN theo
// vm_start, khong chong lan, nam trong [MMAP_BASE, MMAP_TOP).

#ifndef VMA_H
#define VMA_H

#define NVMA 16                    // so VMA toi da moi tien trinh

// Vung dia chi danh cho mmap: [MMAP_BASE, MMAP_TOP)
// (MAXVA, TRAPFRAME lay tu riscv.h / memlayout.h)
#define MMAP_BASE (MAXVA / 2)
#define MMAP_TOP  (TRAPFRAME)

// Chien luoc tim khoang trong
#define VMA_FIRST_FIT 0
#define VMA_BEST_FIT  1

// Bit "dirty" phan mem (dung bit RSW trong PTE) cho MAP_SHARED + PROT_WRITE
#define VMA_PTE_DIRTY (1L << 8)

// 1 = sau moi mmap / munmap / fork, kiem tra tinh toan ven cua danh sach VMA
// (sap xep, khong chong lan, can theo trang, dung so luong) va panic neu sai.
// Dat 0 de tat.
#define VMA_DEBUG 1

struct file;

struct vm_area_struct {
  uint64 vm_start;                 // dia chi dau (can theo trang)
  uint64 vm_end;                   // dia chi cuoi (khong bao gom), can theo trang
  int vm_prot;                     // PROT_READ | PROT_WRITE | PROT_EXEC
  int vm_flags;                    // MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS
  struct file *vm_file;            // file ho tro (0 neu anonymous)
  uint64 vm_pgoff;                 // offset (byte) trong file ung voi vm_start
  struct vm_area_struct *vm_next;  // danh sach lien ket, sap xep tang dan theo vm_start
  int vm_used;                     // slot dang duoc su dung
};

// Chinh sach tim khoang trong hien tai (VMA_FIRST_FIT hoac VMA_BEST_FIT)
extern int vma_policy;

// Kich thuoc (byte) cua mot VMA
#define VMA_LEN(v) ((v)->vm_end - (v)->vm_start)

#endif
