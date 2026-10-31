// vma_test.c - kiem thu vma_mmap / vma_munmap / vma_ctl
// Bien dich: gcc -O2 -Wall vma_test.c -o vma_test [-DNR_VMA_MMAP=472 ...]
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#ifndef NR_VMA_MMAP
#define NR_VMA_MMAP   472
#endif
#ifndef NR_VMA_MUNMAP
#define NR_VMA_MUNMAP 473
#endif
#ifndef NR_VMA_CTL
#define NR_VMA_CTL    474
#endif

#define CTL_DUMP 0
#define CTL_FIT  1
#define CTL_RESET 2
#define ANON_PRIV (MAP_PRIVATE | MAP_ANONYMOUS)

static long pg;
static int fails;

static long vmap(unsigned long len, int prot)
{
	return syscall(NR_VMA_MMAP, 0, len * pg, prot, ANON_PRIV, -1, 0);
}
static long vunmap(unsigned long a, unsigned long len)
{
	return syscall(NR_VMA_MUNMAP, a, len * pg);
}
static void vctl(int cmd, long arg) { syscall(NR_VMA_CTL, cmd, arg); }

static void check(const char *name, int ok)
{
	printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok)
		fails++;
}

int main(void)
{
	long x, a, b, c, p;
	int fit;

	pg = sysconf(_SC_PAGESIZE);
	vctl(CTL_RESET, 0);

	/* 1) Cat / tach: map 16 trang, go giua -> tach doi, go dau, go duoi */
	x = vmap(16, PROT_READ | PROT_WRITE);
	check("mmap tra ve dia chi hop le", x > 0);
	check("munmap giua (SPLIT)", vunmap(x + 4 * pg, 4) == 0);
	check("munmap dau   (cat dau)", vunmap(x, 2) == 0);
	check("munmap duoi  (cat duoi)", vunmap(x + 14 * pg, 2) == 0);
	puts("-> dmesg phai co 2 VMA: [x+2p..x+4p) va [x+8p..x+14p)");
	vctl(CTL_DUMP, 0);
	vctl(CTL_RESET, 0);

	/* 2) First-Fit vs Best-Fit: tao 2 lo trong (8 trang va 3 trang) */
	for (fit = 0; fit <= 1; fit++) {
		vctl(CTL_RESET, 0);
		vctl(CTL_FIT, fit);
		a = vmap(8, PROT_READ | PROT_WRITE);
		vmap(1, PROT_READ);		/* ngan cach (khac co nen khong gop) */
		b = vmap(3, PROT_READ | PROT_WRITE);
		vmap(1, PROT_READ);
		c = vmap(5, PROT_READ | PROT_WRITE);
		vmap(1, PROT_READ);
		vunmap(a, 8);			/* lo 8 trang */
		vunmap(b, 3);			/* lo 3 trang */
		p = vmap(3, PROT_READ | PROT_WRITE);
		if (fit == 0)
			check("First-Fit chon lo dau tien (8 trang)", p == a);
		else
			check("Best-Fit chon lo vua nhat (3 trang)", p == b);
		(void)c;
		vctl(CTL_DUMP, 0);
	}

	/* 3) Kiem tra loi tham so */
	check("len = 0 -> loi", syscall(NR_VMA_MMAP, 0, 0, PROT_READ, ANON_PRIV, -1, 0) == -1);
	check("fd sai (khong anonymous) -> loi",
	      syscall(NR_VMA_MMAP, 0, pg, PROT_READ, MAP_PRIVATE, 9999, 0) == -1);
	check("munmap dia chi le -> loi", syscall(NR_VMA_MUNMAP, 0x10000001UL, pg) == -1);

	/* 4) Ro ri: 500 vong map/unmap, cuoi cung phai con 0 VMA */
	for (int i = 0; i < 500; i++) {
		long t = vmap(4, PROT_READ | PROT_WRITE);
		vunmap(t, 4);
	}
	check("500 vong map/unmap: con 0 VMA", syscall(NR_VMA_CTL, CTL_DUMP, 0) == 0);

	vctl(CTL_FIT, 0);
	vctl(CTL_RESET, 0);
	printf("\nTong ket: %s\n", fails ? "CO LOI" : "TAT CA PASS");
	return fails != 0;
}
