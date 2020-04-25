#include "c2mir-test-data.h"

extern ByVal *byval_ptr_extern(int);

int main() {
  ByVal *ret = byval_ptr_extern(0x89ABCDEF);
  printf("ret ptr: %p\n", ret);
  return ret->value;
}
