#ifndef VMASIM_H
#define VMASIM_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Gia tri trung voi Linux: PROT_READ=1, PROT_WRITE=2, MAP_SHARED=1, MAP_PRIVATE=2, MAP_ANONYMOUS=0x20 */
#define VS_PROT_WRITE   0x2
#define VS_MAP_SHARED   0x01
#define VS_MAP_PRIVATE  0x02
#define VS_MAP_ANON     0x20

struct vmasim_mmap_args {
	__u64 len;
	__u64 offset;
	__u64 result;   /* ra: dia chi ao, hoac 0 neu loi */
	__u32 prot;
	__u32 flags;
	__s32 fd;
	__u32 pad;
};

struct vmasim_munmap_args {
	__u64 addr;
	__u64 len;
};

#define VMASIM_MMAP    _IOWR('v', 1, struct vmasim_mmap_args)
#define VMASIM_MUNMAP  _IOW('v', 2, struct vmasim_munmap_args)
#define VMASIM_POLICY  _IOW('v', 3, int)   /* 0 = First-Fit, 1 = Best-Fit */
#define VMASIM_RESET   _IO('v', 4)

#endif
