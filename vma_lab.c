// SPDX-License-Identifier: GPL-2.0
/*
 * vma_lab.c - Khung thuc thi vma_mmap / vma_munmap (Activity 2, de 17).
 * Dat tai: kernel/vma_lab.c
 *
 * - Moi tien trinh (tgid) co 1 danh sach lien ket cac lab_vma, sap xep tang dan.
 * - Tim cho trong: First-Fit hoac Best-Fit (chon bang vma_ctl).
 * - munmap xu ly 4 kich ban: xoa het, cat dau, cat duoi, tach doi (split).
 * - mmap lien ke, cung co, cung anonymous thi gop (merge) de danh sach gon.
 */
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/mman.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/vma_lab.h>

static LIST_HEAD(lab_spaces);
static DEFINE_MUTEX(lab_lock);
static int lab_fit = VMA_FIRST_FIT;

/* ---------- quan ly khong gian dia chi ---------- */

static struct lab_space *lab_get_space(bool create)
{
	struct lab_space *s;

	list_for_each_entry(s, &lab_spaces, node)
		if (s->tgid == current->tgid)
			return s;
	if (!create)
		return NULL;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return NULL;
	s->tgid = current->tgid;
	INIT_LIST_HEAD(&s->vma_list);
	list_add(&s->node, &lab_spaces);
	return s;
}

static void lab_free_vma(struct lab_space *s, struct lab_vma *v)
{
	list_del(&v->list);
	s->nr_vma--;
	if (v->vm_file)
		fput(v->vm_file);
	kfree(v);
}

/* ---------- tim cho trong: First-Fit / Best-Fit ---------- */

/* Xet 1 khoang trong [gs, ge). Tra ve true neu First-Fit da tim thay (dung som). */
static bool lab_consider(unsigned long gs, unsigned long ge, unsigned long len,
			 unsigned long *best, unsigned long *best_size)
{
	unsigned long size;

	if (ge <= gs)
		return false;
	size = ge - gs;
	if (size < len)
		return false;
	if (size < *best_size) {		/* First-Fit: lan dau luon thoa */
		*best = gs;
		*best_size = size;
	}
	return lab_fit == VMA_FIRST_FIT;
}

/* Tra ve dia chi bat dau khoang trong phu hop, 0 neu khong co. */
static unsigned long lab_find_gap(struct lab_space *s, unsigned long len)
{
	unsigned long prev_end = VMA_LAB_BASE;
	unsigned long best = 0, best_size = ~0UL;
	struct lab_vma *v;

	list_for_each_entry(v, &s->vma_list, list) {
		if (lab_consider(prev_end, v->vm_start, len, &best, &best_size))
			return best;
		prev_end = v->vm_end;
	}
	lab_consider(prev_end, VMA_LAB_LIMIT, len, &best, &best_size);
	return best;
}

/* ---------- chen / gop VMA ---------- */

static void lab_merge(struct lab_space *s, struct lab_vma *v)
{
	struct lab_vma *n;

	if (v->vm_file)			/* chi gop vung anonymous */
		return;

	if (!list_is_last(&v->list, &s->vma_list)) {
		n = list_next_entry(v, list);
		if (!n->vm_file && n->vm_start == v->vm_end &&
		    n->vm_flags == v->vm_flags) {
			v->vm_end = n->vm_end;
			lab_free_vma(s, n);
		}
	}
	if (!list_is_first(&v->list, &s->vma_list)) {
		n = list_prev_entry(v, list);
		if (!n->vm_file && n->vm_end == v->vm_start &&
		    n->vm_flags == v->vm_flags) {
			n->vm_end = v->vm_end;
			lab_free_vma(s, v);
		}
	}
}

static int lab_insert(struct lab_space *s, unsigned long start, unsigned long end,
		      unsigned long flags, struct file *f, unsigned long pgoff)
{
	struct lab_vma *v, *pos;

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->vm_start = start;
	v->vm_end = end;
	v->vm_flags = flags;
	v->vm_file = f;			/* nhan quyen so huu tham chieu file */
	v->vm_pgoff = pgoff;

	list_for_each_entry(pos, &s->vma_list, list)
		if (pos->vm_start >= end)
			break;
	list_add_tail(&v->list, &pos->list);	/* chen truoc pos (hoac cuoi danh sach) */
	s->nr_vma++;

	lab_merge(s, v);
	return 0;
}

/* ---------- go bo khoang [start, end): cat / tach VMA ---------- */

static int lab_unmap_range(struct lab_space *s, unsigned long start,
			   unsigned long end)
{
	struct lab_vma *v, *tmp, *right;

	list_for_each_entry_safe(v, tmp, &s->vma_list, list) {
		if (v->vm_end <= start)
			continue;
		if (v->vm_start >= end)
			break;

		if (v->vm_start >= start && v->vm_end <= end) {
			/* Kich ban 1: bi phu kin -> xoa ca VMA */
			lab_free_vma(s, v);
		} else if (v->vm_start < start && v->vm_end > end) {
			/* Kich ban 2: nam giua -> TACH thanh 2 VMA */
			right = kzalloc(sizeof(*right), GFP_KERNEL);
			if (!right)
				return -ENOMEM;
			right->vm_start = end;
			right->vm_end = v->vm_end;
			right->vm_flags = v->vm_flags;
			right->vm_file = v->vm_file ? get_file(v->vm_file) : NULL;
			right->vm_pgoff = v->vm_pgoff +
					  ((end - v->vm_start) >> PAGE_SHIFT);
			v->vm_end = start;
			list_add(&right->list, &v->list);
			s->nr_vma++;
			break;
		} else if (v->vm_start < start) {
			/* Kich ban 3: cat DUOI (giu phan dau) */
			v->vm_end = start;
		} else {
			/* Kich ban 4: cat DAU (dich vm_start, chinh vm_pgoff) */
			v->vm_pgoff += (end - v->vm_start) >> PAGE_SHIFT;
			v->vm_start = end;
		}
	}
	return 0;
}

static bool lab_range_ok(unsigned long addr, unsigned long len)
{
	return addr >= VMA_LAB_BASE && addr + len > addr &&
	       addr + len <= VMA_LAB_LIMIT;
}

/* ---------- system calls ---------- */

SYSCALL_DEFINE6(vma_mmap, unsigned long, addr, unsigned long, len,
		unsigned long, prot, unsigned long, flags, int, fd,
		unsigned long, off)
{
	struct lab_space *s;
	struct file *f = NULL;
	unsigned long vflags = 0, start;
	long ret;

	if (!len)
		return -EINVAL;
	len = PAGE_ALIGN(len);
	if (!len)
		return -ENOMEM;
	if (offset_in_page(off))
		return -EINVAL;
	/* phai co dung 1 trong MAP_SHARED / MAP_PRIVATE */
	if (!!(flags & MAP_SHARED) == !!(flags & MAP_PRIVATE))
		return -EINVAL;

	if (prot & PROT_READ)
		vflags |= LAB_VM_READ;
	if (prot & PROT_WRITE)
		vflags |= LAB_VM_WRITE;
	if (prot & PROT_EXEC)
		vflags |= LAB_VM_EXEC;
	if (flags & MAP_SHARED)
		vflags |= LAB_VM_SHARED;

	if (!(flags & MAP_ANONYMOUS)) {
		f = fget(fd);
		if (!f)
			return -EBADF;
	}

	mutex_lock(&lab_lock);
	s = lab_get_space(true);
	if (!s) {
		ret = -ENOMEM;
		goto out;
	}

	if (flags & MAP_FIXED) {
		if (offset_in_page(addr) || !lab_range_ok(addr, len)) {
			ret = -EINVAL;
			goto out;
		}
		ret = lab_unmap_range(s, addr, addr + len);	/* de len vung cu */
		if (ret)
			goto out;
		start = addr;
	} else {
		start = lab_find_gap(s, len);			/* First/Best-Fit */
		if (!start) {
			ret = -ENOMEM;
			goto out;
		}
	}

	ret = lab_insert(s, start, start + len, vflags, f, off >> PAGE_SHIFT);
	if (!ret) {
		f = NULL;		/* VMA da giu tham chieu file */
		ret = start;
	}
out:
	mutex_unlock(&lab_lock);
	if (f)
		fput(f);
	return ret;
}

SYSCALL_DEFINE2(vma_munmap, unsigned long, addr, size_t, len)
{
	struct lab_space *s;
	long ret = 0;

	if (offset_in_page(addr) || !len)
		return -EINVAL;
	len = PAGE_ALIGN(len);
	if (!len || !lab_range_ok(addr, len))
		return -EINVAL;

	mutex_lock(&lab_lock);
	s = lab_get_space(false);
	if (s)
		ret = lab_unmap_range(s, addr, addr + len);
	mutex_unlock(&lab_lock);
	return ret;
}

SYSCALL_DEFINE2(vma_ctl, unsigned int, cmd, unsigned long, arg)
{
	struct lab_space *s;
	struct lab_vma *v, *tmp;
	long ret = 0;

	mutex_lock(&lab_lock);
	switch (cmd) {
	case VMA_CTL_SET_FIT:
		if (arg > VMA_BEST_FIT)
			ret = -EINVAL;
		else
			lab_fit = arg;
		break;
	case VMA_CTL_DUMP:
		s = lab_get_space(false);
		pr_info("[vma_lab] tgid=%d nr_vma=%d fit=%s\n", current->tgid,
			s ? s->nr_vma : 0,
			lab_fit == VMA_BEST_FIT ? "best-fit" : "first-fit");
		if (s) {
			list_for_each_entry(v, &s->vma_list, list)
				pr_info("[vma_lab]  [%08lx - %08lx) %c%c%c%c pgoff=%lu %s\n",
					v->vm_start, v->vm_end,
					v->vm_flags & LAB_VM_READ ? 'r' : '-',
					v->vm_flags & LAB_VM_WRITE ? 'w' : '-',
					v->vm_flags & LAB_VM_EXEC ? 'x' : '-',
					v->vm_flags & LAB_VM_SHARED ? 's' : 'p',
					v->vm_pgoff, v->vm_file ? "file" : "anon");
			ret = s->nr_vma;
		}
		break;
	case VMA_CTL_RESET:
		s = lab_get_space(false);
		if (s) {
			list_for_each_entry_safe(v, tmp, &s->vma_list, list)
				lab_free_vma(s, v);
			list_del(&s->node);
			kfree(s);
		}
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&lab_lock);
	return ret;
}
