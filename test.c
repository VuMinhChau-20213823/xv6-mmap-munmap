#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "vmasim.h"

static int dev;

static unsigned long long my_mmap(unsigned long long len, int prot, int flags, int fd)
{
	struct vmasim_mmap_args a = { .len = len, .prot = prot, .flags = flags, .fd = fd };

	if (ioctl(dev, VMASIM_MMAP, &a) < 0) { perror("mmap"); return 0; }
	return a.result;
}

static void my_munmap(unsigned long long addr, unsigned long long len)
{
	struct vmasim_munmap_args a = { .addr = addr, .len = len };

	if (ioctl(dev, VMASIM_MUNMAP, &a) < 0) perror("munmap");
}

static void show(void)
{
	char buf[4096];
	ssize_t n;

	lseek(dev, 0, SEEK_SET);
	n = read(dev, buf, sizeof(buf) - 1);
	if (n > 0) { buf[n] = 0; printf("%s\n", buf); }
}

int main(int argc, char **argv)
{
	int policy = (argc > 1 && atoi(argv[1]) == 1) ? 1 : 0;
	int fd;
	unsigned long long a, b, c, d;

	dev = open("/dev/vmasim", O_RDWR);
	if (dev < 0) { perror("open /dev/vmasim"); return 1; }

	ioctl(dev, VMASIM_RESET);
	ioctl(dev, VMASIM_POLICY, &policy);

	fd = open("/etc/hostname", O_RDONLY);   /* chi lay so fd, module khong doc file */

	a = my_mmap(4096 * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
	b = my_mmap(4096 * 4, PROT_READ, MAP_SHARED, fd);
	c = my_mmap(4096 * 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
	printf("a=0x%llx b=0x%llx c=0x%llx\n", a, b, c);
	show();

	my_munmap(a, 4096 * 2);        /* gap 8KB */
	my_munmap(b + 4096, 4096);     /* gap 4KB, tach b lam 2 VMA */
	show();

	d = my_mmap(4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
	printf("d=0x%llx\n", d);
	show();

	close(fd);
	close(dev);
	return 0;
}
