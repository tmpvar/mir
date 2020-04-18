
#include "mir-setup.h"

#include <c2mir/c2mir.h>
#include <mir-gen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *str;
  uint64_t len;
  uint64_t loc;
} source_code_t;

static uint64_t source_file_mtime(const char *path) {
  uint64_t time = 0;
  #if defined(WIN32)
    WIN32_FILE_ATTRIBUTE_DATA attr;
    int ret = GetFileAttributesExA(
      path,
      GetFileExInfoStandard,
      &attr
    );

    if (!ret) {
      return 0;
    }
    FILETIME mtime = attr.ftLastWriteTime;
    time = ((uint64_t)mtime.dwLowDateTime) + ((uint64_t) mtime.dwHighDateTime) << 32;

  #elif defined(__linux)
    struct stat st = {0};
    if (stat(path, &st) < 0) {
      return 0;
    }

    struct timespec *tv = &st.st_mtim;

    time = (uint64_t)(tv->tv_sec*1000) + (uint64_t)(tv->tv_nsec/1000000);
  #elif defined(__APPLE__)
    struct stat st = {0};
    if (stat(path, &st) < 0) {
      return 0;
    }

    struct timespec *tv = &st.st_mtimespec;
    time = (uint64_t)(tv->tv_sec*1000) + (uint64_t)(tv->tv_nsec/1000000);
  #else
    #error unhandled OS
  #endif

  return time;
}

void source_file_free(source_code_t *code) {
  free(code->str);
  free(code);
}

static source_code_t *source_file_load(const char *path) {
  if (path == NULL) {
    return NULL;
  }

  source_code_t *ret = malloc(sizeof(source_code_t));
  memset(ret, 0, sizeof(source_code_t));

  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }

  if (fseek(f, 0, SEEK_END)) {
    printf("failed to seek to the end\n");
    fclose(f);
    return NULL;
  }
  ret->len = ftell(f);
  if (fseek(f, 0, SEEK_SET)) {
    printf("failed to seek to the beginning\n");
    fclose(f);
    return NULL;
  }

  ret->str = malloc(ret->len + 1);
  ret->len = (uint64_t)fread(ret->str, 1, ret->len, f);
  fclose(f);

  ret->str[ret->len] = 0;
  return ret;
}

void sleep_ms(int ms)
{
#if defined(__linux__) || defined(__APPLE__)
    usleep(ms * 1000);   // usleep takes sleep time in us (1 millionth of a second)
#elif defined(WIN32)
    Sleep(ms);
#else
  #error invalid usleep platform
#endif
}

static void close_std_libs (void) {
  for (int i = 0; i < sizeof (std_libs) / sizeof (lib_t); i++)
    if (std_libs[i].handler != NULL) dlclose (std_libs[i].handler);
}

static void open_std_libs (void) {
  for (int i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    std_libs[i].handler = dlopen (std_libs[i].name, RTLD_LAZY);
}

typedef struct {
  uint64_t mtime;
  char *file;
  source_code_t *code;
} hot_module_t;

static int t_getc (void *data) {
  if (data == NULL) {
    return EOF;
  }
  source_code_t *code = (source_code_t *)data;
  if (code->loc >= code->len || !code->str[code->loc]) {
    return EOF;
  }

  return code->str[code->loc++];
}

// TODO: what is this doing?
typedef const char *char_ptr_t;
DEF_VARR (char_ptr_t);

typedef struct c2mir_macro_command macro_command_t;
DEF_VARR (macro_command_t);

static void *import_resolver (const char *name) {
  void *handler, *sym = NULL;

  for (int i = 0; i < sizeof (std_libs) / sizeof (struct lib); i++)
    if ((handler = std_libs[i].handler) != NULL && (sym = dlsym (handler, name)) != NULL) break;
  // if (sym == NULL)
  //   for (int i = 0; i < VARR_LENGTH (lib_t, cmdline_libs); i++)
  //     if ((handler = VARR_GET (lib_t, cmdline_libs, i).handler) != NULL
  //         && (sym = dlsym (handler, name)) != NULL)
  //       break;
  if (sym == NULL) {
    #ifdef _WIN32
    if (strcmp (name, "LoadLibrary") == 0) return LoadLibrary;
    if (strcmp (name, "FreeLibrary") == 0) return FreeLibrary;
    if (strcmp (name, "GetProcAddress") == 0) return GetProcAddress;
    //if (strcmp (name, "stat") == 0) return stat;
    #else
    if (strcmp (name, "dlopen") == 0) return dlopen;
    if (strcmp (name, "dlclose") == 0) return dlclose;
    if (strcmp (name, "dlsym") == 0) return dlsym;
    if (strcmp (name, "stat") == 0) return stat;
    #endif
    fprintf (stderr, "can not load symbol %s\n", name);
    close_std_libs ();
    return NULL;
  }
  return sym;
}

static double real_usec_time (void) {
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return tv.tv_usec + tv.tv_sec * 1000000.0;
}

int main() {
  struct c2mir_options options = {0};

  VARR (char_ptr_t) * headers;
  VARR_CREATE (char_ptr_t, headers, 0);

  VARR (char_ptr_t) * exec_argv;
  VARR_CREATE (char_ptr_t, exec_argv, 0);

  VARR (macro_command_t) * macro_commands;
  VARR_CREATE (macro_command_t, macro_commands, 0);

  options.include_dirs_num = VARR_LENGTH (char_ptr_t, headers);
  options.include_dirs = VARR_ADDR (char_ptr_t, headers);
  options.macro_commands_num = VARR_LENGTH (macro_command_t, macro_commands);
  options.macro_commands = VARR_ADDR (macro_command_t, macro_commands);

  source_code_t *code;
  MIR_context_t ctx;

  hot_module_t mod = {
    .mtime = 0,
    .file = HOT_RELOAD_SOURCE_FILE,
    .code = NULL
  };

  // LOOP START
  while (1) {
    uint64_t mtime = source_file_mtime(HOT_RELOAD_SOURCE_FILE);
    if (mtime == mod.mtime) {
      sleep_ms(10);
      continue;
    }
    printf("rebuild..\n");

    if (mod.code != NULL) {
      source_file_free(mod.code);
      mod.code = NULL;
    }

    mod.code = source_file_load(HOT_RELOAD_SOURCE_FILE);
    mod.mtime = mtime;

    ctx = MIR_init();
    c2mir_init(ctx);
    if (!c2mir_compile(ctx, &options, t_getc, (void*)mod.code, HOT_RELOAD_SOURCE_FILE, stdout)) {
      printf("failed to compile\n");
      // TODO: wait for change and then go again
      sleep_ms(1000);
      continue;
    }

    {
      MIR_val_t val;
      MIR_module_t module;
      MIR_item_t func, main_func = NULL;
      uint64_t (*fun_addr) (int, void *argv, char *env[]);
      double start_time;

      for (
        module = DLIST_HEAD (MIR_module_t, *MIR_get_module_list (ctx));
        module != NULL;
        module = DLIST_NEXT (MIR_module_t, module)
      ) {
        MIR_load_module (ctx, module);
        for (
          func = DLIST_HEAD (MIR_item_t, module->items);
          func != NULL;
          func = DLIST_NEXT (MIR_item_t, func)
        ) {
          if (func->item_type == MIR_func_item && strcmp (func->u.func->name, "main") == 0) {
            main_func = func;
          }
        }
      }

      if (main_func == NULL) {
        fprintf (stderr, "cannot link program w/o main function\n");
        // TODO: wait for change before continuing
        sleep_ms(1000);
        continue;
      }

      open_std_libs ();
      // TODO: reenable
      // MIR_load_external (ctx, "abort", fancy_abort);
      MIR_load_external (ctx, "_MIR_flush_code_cache", _MIR_flush_code_cache);
      MIR_gen_init (ctx);
      // TODO: reenable
      // if (optimize_level >= 0) MIR_gen_set_optimize_level (ctx, (unsigned) optimize_level);
      // TODO: reenable
      // if (gen_debug_p) MIR_gen_set_debug_file (ctx, stderr);
      MIR_link(
        ctx,
        // TODO: reenable if we want lazy gen
        // gen_exec_p ? MIR_set_gen_interface : MIR_set_lazy_gen_interface,
        MIR_set_gen_interface,
        import_resolver
      );

      fun_addr = MIR_gen (ctx, main_func);
      start_time = real_usec_time ();
      int ret_code = fun_addr(
        VARR_LENGTH(char_ptr_t, exec_argv),
        VARR_ADDR (char_ptr_t, exec_argv),
        // env TODO: this was coming from int main(argc,argv,env).....
        NULL
      );

      if (ret_code) {
        printf("ERROR: main failed with status code: %i\n", ret_code);
      }

      MIR_gen_finish (ctx);
    }
    c2mir_finish(ctx);
    sleep_ms(1000);
  }

  VARR_DESTROY (char_ptr_t, headers);
  VARR_DESTROY (macro_command_t, macro_commands);
  close_std_libs ();
  return 0;
}