// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include "vmasim.h"

#define PGSIZE          4096ULL
#define PGROUNDUP(x)    (((x) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(x)  ((x) & ~(PGSIZE - 1))
#define MMAP_BASE       0x40000000ULL
#define MMAP_END        0x7FFFF000ULL

struct vs_vma {
	u64 start, end;
	u32 prot, flags;
	s32 fd;
	u64 pgoff;
	struct list_head node;
};

static LIST_HEAD(vma_list);          /* sap xep tang dan theo start */
static DEFINE_MUTEX(vma_lock);
static int vma_count;
static int policy;                   /* 0 = First-Fit, 1 = Best-Fit */

/* Xet mot gap. First-Fit: tra true ngay khi gap du lon. Best-Fit: giu gap nho nhat du lon. */
static bool gap_pick(u64 gs, u64 ge, u64 len, u64 *best_addr, u64 *best_size)
{
	if (ge <= gs || ge - gs < len)
		return false;
	if (policy == 0) {
		*best_addr = gs;
		return true;
	}
	if (ge - gs < *best_size) {
		*best_size = ge - gs;
		*best_addr = gs;
	}
	return false;
}

static u64 get_unmapped_area(u64 len)
{
	u64 gap_start = MMAP_BASE, best_addr = 0, best_size = U64_MAX;
	struct vs_vma *v;

	list_for_each_entry(v, &vma_list, node) {
		if (gap_pick(gap_start, v->start, len, &best_addr, &best_size))
			return best_addr;
		if (v->end > gap_start)
			gap_start = v->end;
	}
	gap_pick(gap_start, MMAP_END, len, &best_addr, &best_size);
	return best_addr;                /* 0 neu khong tim thay */
}

static void insert_vma(struct vs_vma *nv)
{
	struct vs_vma *pos;

	list_for_each_entry(pos, &vma_list, node) {
		if (pos->start > nv->start) {
			list_add_tail(&nv->node, &pos->node);   /* chen truoc pos */
			vma_count++;
			return;
		}
	}
	list_add_tail(&nv->node, &vma_list);
	vma_count++;
}

static long do_mmap(struct vmasim_mmap_args *a)
{
	struct vs_vma *v;
	u64 len, va;

	if (a->len == 0 || a->offset % PGSIZE)
		return -EINVAL;
	if (!(a->flags & (VS_MAP_SHARED | VS_MAP_PRIVATE)))
		return -EINVAL;
	if (!(a->flags & VS_MAP_ANON) && a->fd < 0)
		return -EBADF;

	len = PGROUNDUP(a->len);
	va = get_unmapped_area(len);
	if (!va)
		return -ENOMEM;

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;

	v->start = va;
	v->end = va + len;
	v->prot = a->prot;
	v->flags = a->flags;
	v->fd = (a->flags & VS_MAP_ANON) ? -1 : a->fd;
	v->pgoff = a->offset;
	insert_vma(v);

	a->result = va;
	return 0;
}

static long do_munmap(u64 addr, u64 len)
{
	u64 start, end;
	struct vs_vma *cur, *tmp;

	if (len == 0 || addr % PGSIZE)
		return -EINVAL;
	start = addr;
	end = PGROUNDUP(addr + len);

	list_for_each_entry_safe(cur, tmp, &vma_list, node) {
		u64 us, ue;

		if (!(start < cur->end && end > cur->start))
			continue;
		us = max(start, cur->start);
		ue = min(end, cur->end);

		if (us <= cur->start && ue >= cur->end) {          /* A: xoa het */
			list_del(&cur->node);
			vma_count--;
			kfree(cur);
		} else if (us <= cur->start && ue < cur->end) {    /* B1: cat dau */
			if (cur->fd >= 0)
				cur->pgoff += ue - cur->start;
			cur->start = ue;
		} else if (us > cur->start && ue >= cur->end) {    /* B2: cat duoi */
			cur->end = us;
		} else {                                           /* C: tach giua */
			struct vs_vma *nv = kzalloc(sizeof(*nv), GFP_KERNEL);

			if (!nv)
				return -ENOMEM;
			nv->start = ue;
			nv->end = cur->end;
			nv->prot = cur->prot;
			nv->flags = cur->flags;
			nv->fd = cur->fd;
			nv->pgoff = cur->pgoff + (ue - cur->start);
			cur->end = us;
			list_add(&nv->node, &cur->node);               /* chen ngay sau cur */
			vma_count++;
		}
	}
	return 0;
}

static void free_all(void)
{
	struct vs_vma *v, *tmp;

	list_for_each_entry_safe(v, tmp, &vma_list, node) {
		list_del(&v->node);
		kfree(v);
	}
	vma_count = 0;
}

static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	long ret = 0;

	mutex_lock(&vma_lock);
	switch (cmd) {
	case VMASIM_MMAP: {
		struct vmasim_mmap_args a;

		if (copy_from_user(&a, (void __user *)arg, sizeof(a))) { ret = -EFAULT; break; }
		a.result = 0;
		ret = do_mmap(&a);
		if (!ret && copy_to_user((void __user *)arg, &a, sizeof(a)))
			ret = -EFAULT;
		break;
	}
	case VMASIM_MUNMAP: {
		struct vmasim_munmap_args a;

		if (copy_from_user(&a, (void __user *)arg, sizeof(a))) { ret = -EFAULT; break; }
		ret = do_munmap(a.addr, a.len);
		break;
	}
	case VMASIM_POLICY: {
		int p;

		if (copy_from_user(&p, (void __user *)arg, sizeof(p))) { ret = -EFAULT; break; }
		policy = p ? 1 : 0;
		break;
	}
	case VMASIM_RESET:
		free_all();
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&vma_lock);
	return ret;
}

/* cat /dev/vmasim se in danh sach VMA */
static ssize_t vs_read(struct file *f, char __user *ubuf, size_t n, loff_t *pos)
{
	struct vs_vma *v;
	char *buf = kmalloc(PAGE_SIZE * 2, GFP_KERNEL);
	size_t off = 0, cap = PAGE_SIZE * 2;
	int idx = 1;
	ssize_t ret;

	if (!buf)
		return -ENOMEM;

	mutex_lock(&vma_lock);
	off += scnprintf(buf + off, cap - off, "Chinh sach: %s | Tong so VMA: %d\n",
			 policy ? "BEST-FIT" : "FIRST-FIT", vma_count);
	list_for_each_entry(v, &vma_list, node) {
		off += scnprintf(buf + off, cap - off,
				 "VMA %d: [0x%llx - 0x%llx] prot=%u flags=0x%x fd=%d\n",
				 idx++, (unsigned long long)v->start,
				 (unsigned long long)v->end, v->prot, v->flags, v->fd);
	}
	mutex_unlock(&vma_lock);

	ret = simple_read_from_buffer(ubuf, n, pos, buf, off);
	kfree(buf);
	return ret;
}

static const struct file_operations vs_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = vs_ioctl,
	.read           = vs_read,
};

static struct miscdevice vs_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "vmasim",
	.fops  = &vs_fops,
	.mode  = 0666,
};

static int __init vs_init(void)
{
	return misc_register(&vs_dev);
}

static void __exit vs_exit(void)
{
	misc_deregister(&vs_dev);
	mutex_lock(&vma_lock);
	free_all();
	mutex_unlock(&vma_lock);
}

module_init(vs_init);
module_exit(vs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VMA mmap/munmap simulation with First-Fit/Best-Fit");
