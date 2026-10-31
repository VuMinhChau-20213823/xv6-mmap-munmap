/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vma_lab.h - Cau truc VMA (muc 1 cua Activity 2) cho bai tap lon de 17.
 * Dat tai: include/linux/vma_lab.h
 */
#ifndef _LINUX_VMA_LAB_H
#define _LINUX_VMA_LAB_H

#include <linux/list.h>
#include <linux/types.h>

struct file;

/* Co quyen truy cap (vm_flags) - dung bo co rieng de khong phu thuoc doi API mm */
#define LAB_VM_READ     0x1UL
#define LAB_VM_WRITE    0x2UL
#define LAB_VM_EXEC     0x4UL
#define LAB_VM_SHARED   0x8UL

/* Vung dia chi ao dung de mo phong (khong dung toi mm that cua tien trinh) */
#define VMA_LAB_BASE    0x10000000UL
#define VMA_LAB_LIMIT   0x70000000UL

enum vma_fit {
	VMA_FIRST_FIT = 0,
	VMA_BEST_FIT  = 1,
};

enum vma_ctl_cmd {
	VMA_CTL_DUMP    = 0,	/* in danh sach VMA ra dmesg */
	VMA_CTL_SET_FIT = 1,	/* arg: 0 = First-Fit, 1 = Best-Fit */
	VMA_CTL_RESET   = 2,	/* giai phong toan bo VMA cua tien trinh */
};

/* Tuong duong rut gon cua vm_area_struct */
struct lab_vma {
	unsigned long vm_start;		/* dia chi bat dau (bao gom) */
	unsigned long vm_end;		/* dia chi ket thuc (khong bao gom) */
	unsigned long vm_flags;		/* co quyen truy cap LAB_VM_* */
	struct file  *vm_file;		/* NULL neu la anonymous mapping */
	unsigned long vm_pgoff;		/* offset trong file (tinh theo trang) */
	struct list_head list;		/* danh sach lien ket, sap xep theo vm_start */
};

/* Khong gian dia chi ao cua mot tien trinh (khoa theo tgid) */
struct lab_space {
	pid_t tgid;
	int nr_vma;
	struct list_head vma_list;
	struct list_head node;		/* noi vao danh sach cac khong gian */
};

#endif /* _LINUX_VMA_LAB_H */
