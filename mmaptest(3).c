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

// syscall vmainfo(what, arg): quan sat bo dem tham chieu
#define VMAINFO_PGREF      0   // ref cua trang vat ly dang map tai dia chi arg (-1: chua co mat)
#define VMAINFO_FREEPAGES  1   // so trang vat ly dang tu do
#define VMAINFO_FILEREF    2   // ref cua file descriptor arg

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
test_merge(void)
{
  printf("[10] gop VMA: 40 mapping 1 trang lien tiep (vuot NVMA=16)\n");
  char *r[40];
  int ok = 1, contiguous = 1;
  for(int i = 0; i < 40; i++){
    r[i] = mmap(0, PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(failed(r[i])){ ok = 0; break; }
    if(i > 0 && r[i] != r[0] + i * PG) contiguous = 0;
  }
  check(ok, "40 lan mmap deu thanh cong (neu khong gop VMA se het 16 slot)");
  check(ok && contiguous, "cac vung nam lien tiep nhau");
  if(ok){
    for(int i = 0; i < 40; i++) r[i][0] = i + 1;
    int good = 1;
    for(int i = 0; i < 40; i++) if(r[i][0] != i + 1) good = 0;
    check(good, "ghi/doc dung tren ca 40 trang");
    check(munmap(r[0] + 10 * PG, 5 * PG) == 0, "munmap 5 trang giua vung da gop (tach doi)");
    check(r[0][0] == 1 && r[9][0] == 10 && r[15][0] == 16 && r[39][0] == 40,
          "cac trang con lai nguyen ven");
    munmap(r[0], 10 * PG);
    munmap(r[0] + 15 * PG, 25 * PG);
  }
}

static void
test_hint(void)
{
  printf("[11] goi y dia chi addr\n");
  char *b = mmap(0, PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(failed(b)){ check(0, "mmap"); return; }
  char *want = b + 20 * PG;
  char *h = mmap(want, PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(h == want, "vung trong -> kernel dung dung dia chi goi y");
  char *h2 = mmap(want, PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(!failed(h2) && h2 != want, "vung da bi chiem -> kernel chon cho khac (khong loi)");
  if(!failed(h)) munmap(h, PG);
  if(!failed(h2)) munmap(h2, PG);
  munmap(b, PG);
}

static void
test_shared_anon_fork(void)
{
  printf("[12] MAP_SHARED|ANONYMOUS: cha va con dung chung trang vat ly\n");
  char *p = mmap(0, 2 * PG, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(failed(p)){ check(0, "mmap shared anonymous"); return; }
  p[0] = 1;                          // chi cham trang 0 truoc khi fork
  int pid = fork();
  if(pid == 0){
    if(p[0] != 1) exit(1);
    p[0] = 42;                       // trang da co mat
    p[PG] = 43;                      // trang chua cham truoc khi fork
    exit(0);
  }
  int st = -1;
  wait(&st);
  check(st == 0, "con doc dung du lieu cha da ghi");
  check(p[0] == 42, "cha thay NGAY thay doi cua con (trang da co mat)");
  check(p[PG] == 43, "cha thay NGAY thay doi cua con (trang chua cham truoc fork)");
  munmap(p, 2 * PG);
}

static void
test_shared_file_fork(void)
{
  printf("[13] MAP_SHARED file: cha va con chia se trang, du lieu xuong dia dung\n");
  make_file("mm.tmp");
  int fd = open("mm.tmp", O_RDWR);
  char *p = mmap(0, 2 * PG, RW, MAP_SHARED, fd, 0);
  if(failed(p)){ check(0, "mmap file shared"); close(fd); return; }
  char c = p[0];                     // trang 0 da co mat, trang 1 chua
  (void)c;
  int pid = fork();
  if(pid == 0){
    p[10] = 'K';
    p[PG + 10] = 'L';
    exit(0);
  }
  int st = -1;
  wait(&st);
  check(p[10] == 'K' && p[PG + 10] == 'L', "cha thay ngay thay doi cua con truoc khi munmap");
  p[20] = 'M';
  munmap(p, 2 * PG);
  close(fd);

  fd = open("mm.tmp", O_RDONLY);
  p = mmap(0, 2 * PG, PROT_READ, MAP_SHARED, fd, 0);
  check(!failed(p) && p[10] == 'K' && p[PG + 10] == 'L' && p[20] == 'M',
        "ca thay doi cua con va cha deu nam tren file");
  if(!failed(p)) munmap(p, 2 * PG);
  close(fd);
}

static void
test_refcount_alive(void)
{
  printf("[14] reference count: trang khong bi giai phong khi con tien trinh dung\n");
  int fds[2];
  if(pipe(fds) < 0){ check(0, "pipe"); return; }
  char *p = mmap(0, PG, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(failed(p)){ check(0, "mmap"); return; }
  p[0] = 7;
  int pid = fork();
  if(pid == 0){
    char b;
    close(fds[1]);
    read(fds[0], &b, 1);             // cho cha munmap xong
    if(p[0] != 7) exit(1);           // neu trang bi free nham, noi dung se bi ghi de
    p[0] = 8;
    if(p[0] != 8) exit(2);
    exit(0);
  }
  close(fds[0]);
  munmap(p, PG);                     // cha tra tham chieu cua minh
  for(int i = 0; i < 20; i++){       // tao ap luc cap phat: trang bi free nham se bi dung lai
    char *q = mmap(0, 16 * PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(!failed(q)){
      for(int j = 0; j < 16; j++) q[j * PG] = 99;
      munmap(q, 16 * PG);
    }
  }
  write(fds[1], "x", 1);
  close(fds[1]);
  int st = -1;
  wait(&st);
  check(st == 0, "con van doc/ghi duoc trang sau khi cha da munmap");
}

static void
test_share_stress(void)
{
  printf("[15] 300 chu ky fork voi vung shared 100 trang (ro ri / free 2 lan)\n");
  int ok = 1;
  for(int i = 0; i < 300 && ok; i++){
    char *p = mmap(0, 100 * PG, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(failed(p)){ ok = 0; break; }
    for(int j = 0; j < 100; j += 2) p[j * PG] = 1;
    int pid = fork();
    if(pid < 0){ ok = 0; munmap(p, 100 * PG); break; }
    if(pid == 0){
      for(int j = 1; j < 100; j += 2) p[j * PG] = 2;
      exit(0);
    }
    int st = -1;
    wait(&st);
    for(int j = 1; j < 100; j += 2) if(p[j * PG] != 2) ok = 0;
    if(munmap(p, 100 * PG) != 0) ok = 0;
  }
  check(ok, "300 chu ky hoan tat, bo nho khong can (khong ro ri), khong hong");
}

// ---------------------------------------------------------------
// Bang dien bien reference count: mot trang shared qua mmap -> fork x2 ->
// con 1 munmap -> con 2 exit -> cha munmap. verbose = 1 thi in bang.
// Tra ve so kiem tra that bai.
// ---------------------------------------------------------------
static int ref_bad;

static void
ref_row(int verbose, char *step, int ref, int want, int held, int data)
{
  if(verbose){
    printf("  %s | ref=%d | trang dang bi giu=%d | du lieu=%d\n", step, ref, held, data);
  }
  if(ref != want){
    printf("  FAIL: %s: ref=%d, ky vong %d\n", step, ref, want);
    ref_bad++;
  }
}

static int
ref_sequence(int verbose)
{
  int p1[2], p2[2], st = -1;
  char b;

  ref_bad = 0;
  if(pipe(p1) < 0 || pipe(p2) < 0){ printf("  FAIL: pipe\n"); return 1; }

  // trang d: tao san cac trang bang trang trong kernel, de so sanh so trang tu do cho chinh xac
  char *d = mmap(0, PG, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(failed(d)){ printf("  FAIL: mmap\n"); return 1; }
  d[0] = 1;
  int free0 = vmainfo(VMAINFO_FREEPAGES, 0);

  char *p = mmap(0, PG, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(failed(p)){ printf("  FAIL: mmap\n"); return 1; }
  p[0] = 77;
  if(verbose) printf("  Buoc | Thao tac -> ref cua trang p\n");
  ref_row(verbose, "B1 mmap + ghi (1 tien trinh)    ", vmainfo(VMAINFO_PGREF, (uint64)p), 1,
          free0 - vmainfo(VMAINFO_FREEPAGES, 0), p[0]);

  int c1 = fork();
  if(c1 == 0){                       // con 1: thoat bang munmap
    close(p1[1]);
    read(p1[0], &b, 1);
    if(vmainfo(VMAINFO_PGREF, (uint64)p) != 3) exit(1);
    if(p[0] != 77) exit(2);
    if(munmap(p, PG) != 0) exit(3);
    if(vmainfo(VMAINFO_PGREF, (uint64)p) != -1) exit(4);
    exit(0);
  }
  ref_row(verbose, "B2 fork con 1                   ", vmainfo(VMAINFO_PGREF, (uint64)p), 2,
          free0 - vmainfo(VMAINFO_FREEPAGES, 0), p[0]);

  int c2 = fork();
  if(c2 == 0){                       // con 2: thoat bang exit (khong munmap)
    close(p2[1]);
    read(p2[0], &b, 1);
    if(vmainfo(VMAINFO_PGREF, (uint64)p) != 2) exit(1);
    if(p[0] != 77) exit(2);
    exit(0);
  }
  ref_row(verbose, "B3 fork con 2                   ", vmainfo(VMAINFO_PGREF, (uint64)p), 3,
          free0 - vmainfo(VMAINFO_FREEPAGES, 0), p[0]);

  write(p1[1], "x", 1);
  wait(&st);                         // con 1 munmap roi thoat
  if(st != 0){ printf("  FAIL: con 1 bao loi %d\n", st); ref_bad++; }
  ref_row(verbose, "B4 con 1 munmap + thoat         ", vmainfo(VMAINFO_PGREF, (uint64)p), 2,
          free0 - vmainfo(VMAINFO_FREEPAGES, 0), p[0]);

  write(p2[1], "x", 1);
  wait(&st);                         // con 2 exit
  if(st != 0){ printf("  FAIL: con 2 bao loi %d\n", st); ref_bad++; }
  ref_row(verbose, "B5 con 2 exit                   ", vmainfo(VMAINFO_PGREF, (uint64)p), 1,
          free0 - vmainfo(VMAINFO_FREEPAGES, 0), p[0]);
  if(free0 - vmainfo(VMAINFO_FREEPAGES, 0) != 1){
    printf("  FAIL: trang phai van con duoc giu vi cha chua munmap\n");
    ref_bad++;
  }

  munmap(p, PG);
  ref_row(verbose, "B6 cha munmap (nguoi dung cuoi) ", vmainfo(VMAINFO_PGREF, (uint64)p), -1,
          free0 - vmainfo(VMAINFO_FREEPAGES, 0), 0);
  if(free0 - vmainfo(VMAINFO_FREEPAGES, 0) != 0){
    printf("  FAIL: trang chua duoc tra ve sau khi nguoi dung cuoi munmap\n");
    ref_bad++;
  }
  munmap(d, PG);
  close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
  return ref_bad;
}

static void
test_ref_values(void)
{
  printf("[16] bang dien bien reference count (vmainfo)\n");
  check(ref_sequence(0) == 0,
        "ref: 1 -> 2 -> 3 -> 2 -> 1; trang chi duoc tra ve khi nguoi dung cuoi munmap");
}

static void
test_fork_fail(void)
{
  printf("[17] fork that bai (het bo nho): khong ro ri trang va tham chieu file\n");
  make_file("mm.tmp");
  int fd = open("mm.tmp", O_RDWR);
  char *s = mmap(0, 8 * PG, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  int hogpages = 30000;
  char *h = mmap(0, hogpages * PG, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(fd < 0 || failed(s) || failed(h)){ check(0, "chuan bi (open/mmap)"); return; }
  for(int j = 0; j < 8; j++) s[j * PG] = 1;

  int n = 0;                         // chiem gan het bo nho, chua lai khoang 60 trang
  while(n < hogpages && vmainfo(VMAINFO_FREEPAGES, 0) > 60){
    h[n * PG] = 1;
    n++;
  }
  int free_before = vmainfo(VMAINFO_FREEPAGES, 0);
  int ref_s = vmainfo(VMAINFO_PGREF, (uint64)s);
  int ref_fd = vmainfo(VMAINFO_FILEREF, fd);

  int pid = fork();
  if(pid == 0) exit(0);              // chi xay ra neu fork bat ngo thanh cong
  if(pid > 0){
    int st;
    wait(&st);
    check(0, "fork phai that bai khi con khoang 60 trang tu do");
  } else {
    check(1, "fork that bai nhu mong doi khi het bo nho");
  }
  check(vmainfo(VMAINFO_FREEPAGES, 0) == free_before,
        "so trang tu do khong doi sau khi fork that bai (khong ro ri trang)");
  check(vmainfo(VMAINFO_PGREF, (uint64)s) == ref_s,
        "ref cua trang shared tro lai nhu cu (khong tang thua)");
  check(vmainfo(VMAINFO_FILEREF, fd) == ref_fd,
        "ref cua file khong tang (khong ro ri tham chieu file)");

  munmap(h, hogpages * PG);          // tra bo nho, fork phai chay lai binh thuong
  pid = fork();
  if(pid == 0) exit(0);
  int st2 = -1;
  if(pid > 0) wait(&st2);
  check(pid > 0 && st2 == 0, "sau khi giai phong bo nho, fork thanh cong tro lai");
  munmap(s, 8 * PG);
  close(fd);
}

static void
demo_refcount(void)
{
  printf("BANG DIEN BIEN REFERENCE COUNT\n");
  int bad = ref_sequence(1);
  if(bad == 0)
    printf("=> trang chi duoc giai phong khi khong con tien trinh nao dung (B6)\n");
  else
    printf("=> co %d diem sai\n", bad);
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
main(int argc, char *argv[])
{
  if(argc > 1 && strcmp(argv[1], "ref") == 0){
    demo_refcount();               // mmaptest ref: chi in bang dien bien reference count
    exit(0);
  }
  test_anonymous();
  test_shared_file();
  test_private_file();
  test_partial_unmap();
  test_protection();
  test_fork();
  test_two_processes_share_file();
  test_fit_policy();
  test_merge();
  test_hint();
  test_shared_anon_fork();
  test_shared_file_fork();
  test_refcount_alive();
  test_share_stress();
  test_ref_values();
  test_fork_fail();
  test_leak();
  unlink("mm.tmp");
  if(fails == 0)
    printf("\nmmaptest: TAT CA DEU PASS\n");
  else
    printf("\nmmaptest: %d kiem tra THAT BAI\n", fails);
  exit(fails != 0);
}
