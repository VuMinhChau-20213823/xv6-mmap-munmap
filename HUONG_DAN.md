# Hướng dẫn gắn VMA + mmap/munmap vào xv6-riscv

Bộ file này viết cho **xv6-riscv** (bản MIT, chạy trên QEMU). Gồm 4 file mới và một số chỗ cần sửa nhỏ trong file có sẵn.

## 1. File mới (chép vào đúng thư mục)

| File | Nội dung |
|---|---|
| `kernel/vma.h` | `struct vm_area_struct` (vm_start, vm_end, vm_prot, vm_flags, vm_file, vm_pgoff, vm_next) |
| `kernel/vma.c` | danh sách VMA, First-Fit / Best-Fit, mmap, munmap (cắt đầu / cắt đuôi / tách đôi / xóa), lazy fault, write-back, fork |
| `kernel/sysmmap.c` | `sys_mmap`, `sys_munmap`, `sys_vmapolicy` |
| `user/mmaptest.c` | 9 nhóm test |

## 2. Các chỗ cần sửa trong xv6

### kernel/fcntl.h — thêm cờ
```c
#define PROT_NONE     0x0
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void*)-1)
```

### kernel/proc.h
Thêm ở đầu file: `#include "vma.h"`, và thêm 2 trường vào cuối `struct proc`:
```c
  struct vm_area_struct vmas[NVMA];   // bảng VMA của tiến trình
  struct vm_area_struct *vma_head;    // danh sách liên kết, sắp theo địa chỉ
```

### kernel/defs.h — thêm prototype
```c
// vma.c
void            vma_init(struct proc*);
uint64          vma_mmap(struct proc*, uint64, uint64, int, int, int, uint64);
int             vma_munmap(struct proc*, uint64, uint64);
void            vma_unmap_all(struct proc*);
int             vma_fault(struct proc*, uint64, uint64);
int             vma_fork(struct proc*, struct proc*);
```

### kernel/proc.c
1. Trong `allocproc()`, sau khi cấp `p->pagetable` thành công (hoặc ngay sau `p->pid = allocpid();`):
```c
  vma_init(p);
```
2. Trong `fork()`, **sau** `release(&np->lock);` đầu tiên (lúc `pid = np->pid;` xong), thêm:
```c
  if(vma_fork(p, np) < 0){
    acquire(&np->lock);
    freeproc(np);
    release(&np->lock);
    return -1;
  }
```
(Phải gọi khi không giữ spinlock vì có thể ngủ khi truy cập file.)

3. Trong `exit()`, ngay sau dòng `if(p == initproc) panic("init exiting");`:
```c
  vma_unmap_all(p);   // write-back MAP_SHARED, đóng file, giải phóng trang
```

### kernel/exec.c
Trong `exec()`, ngay sau phần khai báo biến và **trước** `begin_op();`:
```c
  vma_unmap_all(p);
```
(`p` là `myproc()`; nếu file của bạn chưa có biến `p` ở đầu hàm thì thêm `struct proc *p = myproc();`.)

### kernel/trap.c — `usertrap()`
Thêm một nhánh `else if` **trước** nhánh `else` cuối cùng (sau nhánh `devintr()`):
```c
  } else if(r_scause() == 12 || r_scause() == 13 || r_scause() == 15){
    intr_on();                       // cần bật ngắt vì có thể ngủ khi đọc file
    if(vma_fault(p, r_stval(), r_scause()) < 0)
      setkilled(p);                  // bản xv6 cũ: p->killed = 1;
  } else {
```

### kernel/vm.c — `walkaddr()` (để read()/write() vào vùng mmap chưa fault vẫn chạy)
Thêm `#include "spinlock.h"` và `#include "proc.h"` sau `#include "fs.h"`, rồi thay phần kiểm tra PTE:
```c
  pte = walk(pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0){
    struct proc *p = myproc();
    if(p && p->pagetable == pagetable && vma_fault(p, va, 0) == 0)
      pte = walk(pagetable, va, 0);
    else
      return 0;
  }
  if((*pte & PTE_U) == 0)
    return 0;
```

### Đăng ký system call
- `kernel/syscall.h` (bản của bạn đã dùng đến `SYS_hello 23`, nên số mới bắt đầu từ 24):
```c
#define SYS_mmap      24
#define SYS_munmap    25
#define SYS_vmapolicy 26
```
- `kernel/syscall.c`:
```c
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
extern uint64 sys_vmapolicy(void);
...
[SYS_mmap]      sys_mmap,
[SYS_munmap]    sys_munmap,
[SYS_vmapolicy] sys_vmapolicy,
```
- `user/usys.pl`:
```perl
entry("mmap");
entry("munmap");
entry("vmapolicy");
```
- `user/user.h`:
```c
void *mmap(void *addr, uint64 length, int prot, int flags, int fd, uint64 offset);
int munmap(void *addr, uint64 length);
int vmapolicy(int policy);
```
- `Makefile`: thêm `$K/vma.o \` và `$K/sysmmap.o \` vào `OBJS`, và `$U/_mmaptest\` vào `UPROGS`.

## 3. Build và chạy
```
make clean
make qemu
$ mmaptest
```
Nếu `make` báo lỗi nhầm số tham số của `argint`/`argaddr`: bản xv6 cũ trả về `int`, bản mới trả về `void`; code đã viết để chạy được với cả hai.

## 4. Đối chiếu với yêu cầu trong ảnh

| Yêu cầu | Nằm ở đâu |
|---|---|
| Quản lý VMA, `vm_area_struct` với các thuộc tính cốt lõi | `vma.h`, danh sách trong `struct proc` |
| Khung `sys_mmap`, `sys_munmap` | `sysmmap.c` + `vma_mmap`, `vma_munmap` |
| First-Fit và Best-Fit | `vma_find_gap()`; đổi bằng `vmapolicy(0/1)` (mặc định Best-Fit) |
| Cắt, tách, dịch chuyển VMA, giữ danh sách liên kết nhất quán | `vma_munmap()`: xóa / cắt đầu (vm_start và vm_pgoff dịch) / cắt đuôi / tách đôi; kiểm tra đủ slot trước khi sửa |

## 5. Giới hạn cần biết
- `addr` truyền vào `mmap` chỉ là gợi ý và bị bỏ qua; kernel luôn chọn vùng trong `[MMAP_BASE, MMAP_TOP)` = `[MAXVA/2, TRAPFRAME)`.
- Tối đa `NVMA = 16` VMA mỗi tiến trình.
- `MAP_SHARED` có file: dirty được theo dõi bằng bit phần mềm (map read-only rồi bắt lỗi ghi đầu tiên) và ghi xuống đĩa khi `munmap` / `exit` / `fork`. Chưa chia sẻ trang vật lý giữa các tiến trình (phần mở rộng cần thêm đếm tham chiếu trong `kalloc.c`), nên hai tiến trình thấy thay đổi của nhau sau khi bên kia `munmap`, không phải tức thì.
- `MAP_SHARED | MAP_ANONYMOUS` khi `fork` hiện được sao chép như private.
- Nếu kernel tự ghi vào trang shared qua `read()` (copyout) trước khi trang bị ghi bằng lệnh user, thay đổi đó không được đánh dấu dirty.
- Không có `msync`, `mprotect`, `MAP_FIXED`.
