#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>  // portable: uint64_t   MSVC: __int64
typedef struct timeval {
  long tv_sec;
  long tv_usec;
} timeval;
int gettimeofday (struct timeval *tp, struct timezone *tzp) {
  // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
  // This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
  // until 00:00:00 January 1, 1970
  static const uint64_t EPOCH = ((uint64_t) 116444736000000000ULL);

  SYSTEMTIME system_time;
  FILETIME file_time;
  uint64_t time;

  GetSystemTime (&system_time);
  SystemTimeToFileTime (&system_time, &file_time);
  time = ((uint64_t) file_time.dwLowDateTime);
  time += ((uint64_t) file_time.dwHighDateTime) << 32;

  tp->tv_sec = (long) ((time - EPOCH) / 10000000L);
  tp->tv_usec = (long) (system_time.wMilliseconds * 1000);
  return 0;
}
#else
#include <dlfcn.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif
#endif

struct lib {
  char *name;
  void *handler;
};

typedef struct lib lib_t;

#if defined(__unix__) || defined(__linux__)

  #if UINTPTR_MAX == 0xffffffff
    static lib_t std_libs[] = {
      {"/lib/libc.so.6", NULL},
      {"/lib32/libc.so.6", NULL},
      {"/lib/libm.so.6", NULL},
      {"/lib32/libm.so.6", NULL}
    };
    static const char *std_lib_dirs[] = {"/lib", "/lib32"};
  #elif UINTPTR_MAX == 0xffffffffffffffff
    #if defined(__x86_64__)
      static lib_t std_libs[] = {
        {"/lib64/libc.so.6", NULL},
        {"/lib/x86_64-linux-gnu/libc.so.6", NULL},
        {"/lib64/libm.so.6", NULL},
        {"/lib/x86_64-linux-gnu/libm.so.6", NULL}
      };
      static const char *std_lib_dirs[] = {"/lib64", "/lib/x86_64-linux-gnu"};
    #elif (__aarch64__)
      static lib_t std_libs[] = {
        {"/lib64/libc.so.6", NULL},
        {"/lib/aarch64-linux-gnu/libc.so.6", NULL},
        {"/lib64/libm.so.6", NULL},
        {"/lib/aarch64-linux-gnu/libm.so.6", NULL}
      };
      static const char *std_lib_dirs[] = {"/lib64", "/lib/aarch64-linux-gnu"};
    #elif (__PPC64__)
      static lib_t std_libs[] = {
        {"/lib64/libc.so.6", NULL},
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
          {"/lib/powerpc64le-linux-gnu/libc.so.6", NULL},
        #else
          {"/lib/powerpc64-linux-gnu/libc.so.6", NULL},
        #endif
        {"/lib64/libm.so.6", NULL},
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
          {"/lib/powerpc64le-linux-gnu/libm.so.6", NULL},
        #else
          {"/lib/powerpc64-linux-gnu/libm.so.6", NULL},
        #endif
      };

      static const char *std_lib_dirs[] = {
        "/lib64",
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
			  "/lib/powerpc64le-linux-gnu",
        #else
				"/lib/powerpc64-linux-gnu",
        #endif
      };
    #else
      #error cannot recognize 32- or 64-bit target
    #endif
  #endif
  static const char *lib_suffix = ".so";
#elif defined(WIN32)
  static lib_t std_libs[] = {{"msvcrt.dll", NULL}, {"kernel.dll", NULL}};
  static const char *std_lib_dirs[] = {NULL};
  static const char *lib_suffix = ".dll";
  #define dlopen(n, na) LoadLibrary(n)
  #define dlclose(n) FreeLibrary(n)
  #define dlsym(a, b) GetProcAddress(a,b)
#endif

#ifdef _WIN32
static const int slash = '\\';
#else
static const int slash = '/';
#endif

#if defined(__APPLE__)
static lib_t std_libs[] = {{"/usr/lib/libc.dylib", NULL}, {"/usr/lib/libm.dylib", NULL}};
static const char *std_lib_dirs[] = {"/usr/lib"};
static const char *lib_suffix = ".dylib";
#endif
