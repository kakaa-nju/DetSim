#include <sys/time.h>
#define NULL (void *)0

__attribute__((noinline)) int call1() { 
  struct timeval tv;
  gettimeofday(&tv, NULL); 
  return tv.tv_sec; 
}
__attribute__((noinline)) int call2() { return call1(); }
__attribute__((noinline)) int call3() { return call2(); }
__attribute__((noinline)) int call4() { return call3(); }
__attribute__((noinline)) int call5() { return call4(); }
__attribute__((noinline)) int call6() { return call5(); }
__attribute__((noinline)) int call7() { return call6(); }
__attribute__((noinline)) int call8() { return call7(); }
__attribute__((noinline)) int call9() { return call8(); }
__attribute__((noinline)) int call10() { return call9(); }
__attribute__((noinline)) int call11() { return call10(); }
__attribute__((noinline)) int call12() { return call11(); }
__attribute__((noinline)) int call13() { return call12(); }
__attribute__((noinline)) int call14() { return call13(); }
__attribute__((noinline)) int call15() { return call14(); }
__attribute__((noinline)) int call16() { return call15(); }
__attribute__((noinline)) int call17() { return call16(); }
__attribute__((noinline)) int call18() { return call17(); }
__attribute__((noinline)) int call19() { return call18(); }
__attribute__((noinline)) int call20() { return call19(); }
__attribute__((noinline)) int call21() { return call20(); }
__attribute__((noinline)) int call22() { return call21(); }
__attribute__((noinline)) int call23() { return call22(); }
__attribute__((noinline)) int call24() { return call23(); }
__attribute__((noinline)) int call25() { return call24(); }
__attribute__((noinline)) int call26() { return call25(); }
__attribute__((noinline)) int call27() { return call26(); }
__attribute__((noinline)) int call28() { return call27(); }
__attribute__((noinline)) int call29() { return call28(); }
__attribute__((noinline)) int call30() { return call29(); }
__attribute__((noinline)) int call31() { return call30(); }
__attribute__((noinline)) int call32() { return call31(); }

int main() {
  while (1) call32();
}
