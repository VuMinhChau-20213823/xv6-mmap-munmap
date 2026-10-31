#!/usr/bin/env bash
# apply_vma_lab.sh - cai vma_lab vao kernel source va khai bao 3 system call.
# Dung: bash apply_vma_lab.sh [duong_dan_kernel]   (mac dinh ~/gpu_checker/linux-7.2.6)
set -euo pipefail

KDIR="${1:-$HOME/gpu_checker/linux-7.2.6}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TBL="$KDIR/arch/x86/entry/syscalls/syscall_64.tbl"
SYSH="$KDIR/include/linux/syscalls.h"

[ -f "$KDIR/Makefile" ] || { echo "Khong thay kernel source: $KDIR"; exit 1; }
[ -f "$TBL" ] || { echo "Khong thay $TBL"; exit 1; }

# 1) Copy file nguon
install -m 644 "$HERE/vma_lab.h" "$KDIR/include/linux/vma_lab.h"
install -m 644 "$HERE/vma_lab.c" "$KDIR/kernel/vma_lab.c"

# 2) Them vao kernel/Makefile
grep -q 'vma_lab\.o' "$KDIR/kernel/Makefile" || echo 'obj-y += vma_lab.o' >> "$KDIR/kernel/Makefile"

# 3) Khai bao syscall: bang syscall + prototype trong syscalls.h
python3 - "$TBL" "$SYSH" <<'PY'
import re, sys
tbl, sysh = sys.argv[1], sys.argv[2]

lines = open(tbl).read().split('\n')
if not any(re.search(r'\bvma_mmap\b', l) for l in lines):
    idx = [i for i, l in enumerate(lines)
           if re.match(r'^\d+\s', l) and int(l.split()[0]) < 512]
    last = max(idx, key=lambda i: int(lines[i].split()[0]))
    n = int(lines[last].split()[0])
    new = [f"{n+1}\tcommon\tvma_mmap\t\tsys_vma_mmap",
           f"{n+2}\tcommon\tvma_munmap\t\tsys_vma_munmap",
           f"{n+3}\tcommon\tvma_ctl\t\t\tsys_vma_ctl"]
    lines[last+1:last+1] = new
    open(tbl, 'w').write('\n'.join(lines))
    print(f"Da them syscall so {n+1}, {n+2}, {n+3} vao syscall_64.tbl")
else:
    print("syscall_64.tbl da co vma_mmap - giu nguyen")

src = open(sysh).read()
if 'sys_vma_mmap' not in src:
    decl = (
        "\n/* vma_lab (bai tap lon de 17) */\n"
        "asmlinkage long sys_vma_mmap(unsigned long addr, unsigned long len,\n"
        "\t\t\t     unsigned long prot, unsigned long flags,\n"
        "\t\t\t     int fd, unsigned long off);\n"
        "asmlinkage long sys_vma_munmap(unsigned long addr, size_t len);\n"
        "asmlinkage long sys_vma_ctl(unsigned int cmd, unsigned long arg);\n\n"
    )
    i = src.rfind('#endif')
    open(sysh, 'w').write(src[:i] + decl + src[i:])
    print("Da them prototype vao include/linux/syscalls.h")
else:
    print("syscalls.h da co sys_vma_mmap - giu nguyen")
PY

echo "--- Cac dong syscall hien co:"
grep -nE 'vma_(mmap|munmap|ctl)' "$TBL"
echo "Xong. Nho dung dung so syscall o tren khi bien dich vma_test.c"
