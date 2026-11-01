// user/mmaptest.c
// Chuong trinh kiem thu mmap / munmap / VMA / First-Fit / Best-Fit tren xv6.
// Chay: $ mmaptest

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PG 4096
#define RW (PROT_READ | PROT_WRITE)
#define VMA_FIRST_FIT_ARG 0
#define VMA_BEST_FIT_ARG  1

static int fails = 0;

static void
check(int cond, char *msg)
{
  if(cond){
    printf("  ok:   %s\n", msg);
  } else {
    printf("  FAIL: %s\n", msg);
    fails++;
  }
}

static int
failed(void *p)
{
  return p == MAP_FAILED;
}

// Tao file test 2 trang: trang 0 toan 'A', trang 1 toan 'B'.
static void
make_file(char *name)
{
  static char buf[PG];
  int fd = open(name, O_CREATE | O_RDWR | O_TRUNC);
  if(fd < 0){ printf("cannot create %s\n", name); exit(1); }
  memset(buf, 'A', PG);
  write(fd, buf, PG);
  memset(buf, 'B', PG);
  write(fd, buf, PG);
  close(fd);
}

static void
test_anonymous(void)
{
  printf("[1] anonymous mapping (lazy, zero-filled)\n");
  char *p = mmap(0, 3 * PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(!failed(p), "mmap anonymous thanh cong");
  if(failed(p)) return;
  check(p[0] == 0 && p[PG + 5] == 0 && p[3 * PG - 1] == 0, "noi dung ban dau = 0");
  p[0] = 'x'; p[2 * PG] = 'y';
  check(p[0] == 'x' && p[2 * PG] == 'y', "ghi roi doc lai dung");
  check(munmap(p, 3 * PG) == 0, "munmap thanh cong");
}

static void
test_shared_file(void)
{
  printf("[2] MAP_SHARED: ghi qua mapping, du lieu xuong dia sau munmap\n");
  char buf[8];
  make_file("mm.tmp");
  int fd = open("mm.tmp", O_RDWR);
  char *p = mmap(0, 2 * PG, RW, MAP_SHARED, fd, 0);
  check(!failed(p), "mmap file shared");
  if(failed(p)) return;
  check(p[0] == 'A' && p[PG] == 'B', "doc dung noi dung file");
  p[10] = 'Z';
  p[PG + 20] = 'Q';
  munmap(p, 2 * PG);
  close(fd);

  fd = open("mm.tmp", O_RDONLY);
  read(fd, buf, 8);
  check(buf[0] == 'A', "byte chua sua van la 'A'");
  char c = 0;
  close(fd);

  // doc lai byte 10 va byte PG+20 bang mapping read-only moi
  fd = open("mm.tmp", O_RDONLY);
  p = mmap(0, 2 * PG, PROT_READ, MAP_SHARED, fd, 0);
  c = failed(p) ? 0 : p[10];
  check(c == 'Z', "byte 10 = 'Z' sau khi munmap (da write-back)");
  c = failed(p) ? 0 : p[PG + 20];
  check(c == 'Q', "byte PG+20 = 'Q' (write-back trang thu hai)");
  if(!failed(p)) munmap(p, 2 * PG);
  close(fd);
}

static void
test_private_file(void)
{
  printf("[3] MAP_PRIVATE: ghi KHONG xuong file\n");
  make_file("mm.tmp");
  int fd = open("mm.tmp", O_RDWR);
  char *p = mmap(0, PG, RW, MAP_PRIVATE, fd, 0);
  check(!failed(p), "mmap file private");
  if(failed(p)) return;
  p[0] = 'X';
  check(p[0] == 'X', "thay doi nhin thay trong tien trinh");
  munmap(p, PG);
  close(fd);

  fd = open("mm.tmp", O_RDONLY);
  p = mmap(0, PG, PROT_READ, MAP_PRIVATE, fd, 0);
  check(!failed(p) && p[0] == 'A', "file tren dia van la 'A'");
  if(!failed(p)) munmap(p, PG);
  close(fd);
}

static void
test_partial_unmap(void)
{
  printf("[4] munmap tung phan: tach doi / cat dau / cat duoi\n");
  char *p = mmap(0, 4 * PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(!failed(p), "mmap 4 trang");
  if(failed(p)) return;
  for(int i = 0; i < 4; i++) p[i * PG] = '0' + i;

  check(munmap(p + PG, PG) == 0, "munmap trang giua (tach doi VMA)");
  check(p[0] == '0' && p[2 * PG] == '2' && p[3 * PG] == '3', "3 trang con lai nguyen ven");

  int pid = fork();
  if(pid == 0){
    p[PG] = 1;                      // vung vua go -> phai bi kill
    printf("  FAIL: truy cap vung da munmap khong bi chan\n");
    exit(0);
  }
  int st = 0;
  wait(&st);
  check(st != 0, "truy cap vao lo trong bi kernel kill");

  check(munmap(p, PG) == 0, "munmap trang dau (cat dau)");
  check(munmap(p + 3 * PG, PG) == 0, "munmap trang cuoi (cat duoi)");
  check(p[2 * PG] == '2', "trang 2 con nguyen");
  check(munmap(p + 2 * PG, PG) == 0, "munmap not trang con lai (xoa VMA)");
  check(munmap(p + 2 * PG, PG) == 0, "munmap vung da trong khong loi");
}

static void
test_protection(void)
{
  printf("[5] bao ve: ghi vao vung PROT_READ bi kill\n");
  char *p = mmap(0, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(!failed(p), "mmap read-only");
  if(failed(p)) return;
  check(p[0] == 0, "doc duoc");
  int pid = fork();
  if(pid == 0){
    p[0] = 1;
    printf("  FAIL: ghi vao vung read-only khong bi chan\n");
    exit(0);
  }
  int st = 0;
  wait(&st);
  check(st != 0, "ghi vao vung read-only bi kill");
  munmap(p, PG);
}

static void
test_fork(void)
{
  printf("[6] fork: VMA duoc sao chep (private -> doc lap)\n");
  char *p = mmap(0, PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(failed(p)){ check(0, "mmap"); return; }
  p[0] = 7;
  int pid = fork();
  if(pid == 0){
    if(p[0] != 7) exit(1);
    p[0] = 9;
    if(p[0] != 9) exit(2);
    exit(0);
  }
  int st = -1;
  wait(&st);
  check(st == 0, "con thay du lieu cua cha va tu ghi duoc");
  check(p[0] == 7, "cha khong bi anh huong boi con");
  munmap(p, PG);
}

static void
test_two_processes_share_file(void)
{
  printf("[7] hai tien trinh cung sua mot file qua mmap\n");
  make_file("mm.tmp");
  int pid = fork();
  if(pid == 0){
    int fd = open("mm.tmp", O_RDWR);
    char *p = mmap(0, PG, RW, MAP_SHARED, fd, 0);
    if(failed(p)) exit(1);
    p[100] = 'C';
    munmap(p, PG);
    close(fd);
    exit(0);
  }
  int st = -1;
  wait(&st);
  int fd = open("mm.tmp", O_RDWR);
  char *p = mmap(0, PG, RW, MAP_SHARED, fd, 0);
  check(!failed(p) && p[100] == 'C', "tien trinh cha thay thay doi cua con");
  if(!failed(p)){
    p[200] = 'P';
    munmap(p, PG);
  }
  close(fd);
  fd = open("mm.tmp", O_RDONLY);
  p = mmap(0, PG, PROT_READ, MAP_SHARED, fd, 0);
  check(!failed(p) && p[100] == 'C' && p[200] == 'P', "ca hai thay doi nam tren file");
  if(!failed(p)) munmap(p, PG);
  close(fd);
}

static void
test_fit_policy(void)
{
  printf("[8] First-Fit vs Best-Fit\n");
  char *r[5];
  for(int i = 0; i < 5; i++){
    int len = (i == 1) ? 3 * PG : PG;       // r1 dai 3 trang
    r[i] = mmap(0, len, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(failed(r[i])){ check(0, "mmap r[i]"); return; }
  }
  munmap(r[1], 3 * PG);                     // lo trong 3 trang
  munmap(r[3], PG);                         // lo trong 1 trang

  vmapolicy(VMA_FIRST_FIT_ARG);
  char *x = mmap(0, PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(x == r[1], "First-Fit chon lo trong DAU TIEN (3 trang)");
  munmap(x, PG);

  vmapolicy(VMA_BEST_FIT_ARG);
  char *y = mmap(0, PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(y == r[3], "Best-Fit chon lo trong VUA KHIT NHAT (1 trang)");
  munmap(y, PG);

  munmap(r[0], PG);
  munmap(r[2], PG);
  munmap(r[4], PG);
}

static void
test_leak(void)
{
  printf("[9] 500 chu ky mmap/touch/munmap (kiem tra ro ri bo nho)\n");
  int ok = 1;
  for(int i = 0; i < 500; i++){
    char *p = mmap(0, 100 * PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(failed(p)){ ok = 0; break; }
    for(int j = 0; j < 100; j++) p[j * PG] = 1;
    if(munmap(p, 100 * PG) != 0){ ok = 0; break; }
  }
  check(ok, "500 chu ky hoan tat, khong het bo nho");
}

int
main(void)
{
  test_anonymous();
  test_shared_file();
  test_private_file();
  test_partial_unmap();
  test_protection();
  test_fork();
  test_two_processes_share_file();
  test_fit_policy();
  test_leak();
  unlink("mm.tmp");
  if(fails == 0)
    printf("\nmmaptest: TAT CA DEU PASS\n");
  else
    printf("\nmmaptest: %d kiem tra THAT BAI\n", fails);
  exit(fails != 0);
}
