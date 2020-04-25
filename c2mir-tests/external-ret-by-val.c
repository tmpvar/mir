#include "c2mir-test-data.h"

extern ByVal byval_extern(int);

int main() {
  ByVal ret = byval_extern(0x89ABCDEF);
  printf("ret ptr: %p, val: %x\n", &ret, ret.value);
  return ret.value;
}
