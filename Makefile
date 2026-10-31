obj-m += vmasim.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	gcc -Wall -I. test.c -o test

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test
