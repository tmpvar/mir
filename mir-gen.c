/* This file is a part of MIR project.
   Copyright (C) 2018-2020 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

/* Optimization pipeline:

            ----------------      -----------      ------------------      -------------
   MIR --->|    Simplify    |--->| Build CFG |--->|  Common Sub-Expr |--->|  Dead Code  |
            ----------------      -----------     |    Elimination   |	  | Elimination |
                                                   ------------------	   -------------
                                                                                  |
                                                                                  V
       ----------------     ------------     ---------     -----------     ------------
      | Loop Invariant |   |  Reaching  |   | Finding |   | Variable  |   |  Reaching  |
      | Code Motion    |<--| Definitons |<--|  Loops  |<--| Renaming  |<--| Definitons |
       ----------------    |  Analysis  |    ---------     -----------    |  Analysis  |
              |             ------------                                   ------------
              V
    ----------------------     -----------     ---------     ------------     -------------
   |  Sparse Conditional  |-->| Machinize |-->| Finding |-->| Build Live |-->| Build Live  |
   | Constant Propagation |    -----------    |  Loops  |   |    Info    |   |   Ranges    |
    ----------------------                     ---------     ------------     -------------
                                                                                    |
                                                                                    V
               ---------------     -------------     ---------     ---------     ---------
   Machine <--|   Generate    |<--|  Dead Code  |<--| Combine |<--| Rewrite |<--|  Assign |
    Insns     | machine insns |   | Elimination |    ---------     ---------     ---------
               ---------------     ------------


   Simplify: Lowering MIR (in mir.c).
   Build CGF: Building Control Flow Graph (basic blocks and CFG edges).
   Common Sub-Expression Elimination: Reusing calculated values (it is before SCCP because
                                      we need the right value numbering after simplification)
   Dead code elimination: Removing insns with unused outputs.
   Reaching definition Analysis: Analysis required for subsequent variable renaming
   Variable renaming: Rename disjoint live ranges of variables which is beneficial for
                      register allocation and loop invariant code motion.
   Finding Loops: Building loop tree which is used in subsequent loop invariant code motion.
   Reaching definition Analysis: Analysis required for subsequent loop invariant code motion
   Loop Invariant Code Motion (LICM): Register pressure sensitive moves loop invariant insns out
                                      of the loop.
   Sparse Conditional Constant Propagation: constant propagation and removing death paths of CFG
   Machinize: Machine-dependent code (e.g. in mir-gen-x86_64.c)
              transforming MIR for calls ABI, 2-op insns, etc.
   Finding Loops: Building loop tree which is used in subsequent register allocation.
   Building Live Info: Calculating live in and live out for the basic blocks.
   Build Live Ranges: Calculating program point ranges for registers.
   Assign: Priority-based assigning hard regs and stack slots to registers.
   Rewrite: Transform MIR according to the assign using reserved hard regs.
   Combine (code selection): Merging data-depended insns into one.
   Dead code elimination: Removing insns with unused outputs.
   Generate machine insns: Machine-dependent code (e.g. in
                           mir-gen-x86_64.c) creating machine insns.

   Terminology:
   reg - MIR (pseudo-)register (their numbers are in MIR_OP_REG and MIR_OP_MEM)
   hard reg - MIR hard register (their numbers are in MIR_OP_HARD_REG and MIR_OP_HARD_REG_MEM)
   breg (based reg) - function pseudo registers whose numbers start with zero
   var - pseudo and hard register (var numbers for psedo-registers
         are based reg numbers + MAX_HARD_REG + 1)
   loc - hard register and stack locations (stack slot numbers start with MAX_HARD_REG + 1).

   We don't use SSA because the optimization pipeline could use SSA is
   short (2 passes) and going into / out of SSA is expensive.
*/

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <assert.h>
#ifdef _WIN32
#define __x86_64__ 1
#endif
#ifdef NDEBUG
static inline int gen_assert (int cond) { return 0 && cond; }
#else
#define gen_assert(cond) assert (cond)
#endif

struct MIR_context;
static void util_error (struct MIR_context *ctx, const char *message);
static void varr_error (const char *message) { util_error (NULL, message); }
#define MIR_VARR_ERROR varr_error

#include "mir.h"
#include "mir-dlist.h"
#include "mir-bitmap.h"
#include "mir-htab.h"
#include "mir-hash.h"
#include "mir-gen.h"

static void MIR_NO_RETURN util_error (MIR_context_t ctx, const char *message) {
  (*MIR_get_error_func (ctx)) (MIR_alloc_error, message);
}

static void *gen_malloc (MIR_context_t ctx, size_t size) {
  void *res = malloc (size);
  if (res == NULL) util_error (ctx, "no memory");
  return res;
}

static MIR_reg_t gen_new_temp_reg (MIR_context_t ctx, MIR_type_t type, MIR_func_t func);
static void set_label_disp (MIR_insn_t insn, size_t disp);
static size_t get_label_disp (MIR_insn_t insn);
static void create_new_bb_insns (MIR_context_t ctx, MIR_insn_t before, MIR_insn_t after,
                                 MIR_insn_t insn_for_bb);
static void gen_delete_insn (MIR_context_t ctx, MIR_insn_t insn);
static void gen_add_insn_before (MIR_context_t ctx, MIR_insn_t before, MIR_insn_t insn);
static void gen_add_insn_after (MIR_context_t ctx, MIR_insn_t after, MIR_insn_t insn);
static void setup_call_hard_reg_args (MIR_insn_t call_insn, MIR_reg_t hard_reg);

#ifndef MIR_GEN_CALL_TRACE
#define MIR_GEN_CALL_TRACE 0
#endif

typedef struct func_cfg *func_cfg_t;

struct target_ctx;
struct data_flow_ctx;
struct cse_ctx;
struct rdef_ctx;
struct rename_ctx;
struct licm_ctx;
struct ccp_ctx;
struct lr_ctx;
struct ra_ctx;
struct selection_ctx;

typedef struct loop_node *loop_node_t;
DEF_VARR (loop_node_t);

struct gen_ctx {
  unsigned int optimize_level; /* 0:only RA; 1:+combiner; 2: +CSE/CCP (default); >=3: everything  */
  MIR_item_t curr_func_item;
#if !MIR_NO_GEN_DEBUG
  FILE *debug_file;
#endif
  bitmap_t insn_to_consider, temp_bitmap, temp_bitmap2, all_vars;
  bitmap_t call_used_hard_regs;
  func_cfg_t curr_cfg;
  size_t curr_bb_index, curr_loop_node_index;
  struct target_ctx *target_ctx;
  struct data_flow_ctx *data_flow_ctx;
  struct cse_ctx *cse_ctx;
  struct rename_ctx *rename_ctx;
  struct licm_ctx *licm_ctx;
  struct rdef_ctx *rdef_ctx;
  struct ccp_ctx *ccp_ctx;
  struct lr_ctx *lr_ctx;
  struct ra_ctx *ra_ctx;
  struct selection_ctx *selection_ctx;
  VARR (loop_node_t) * loop_nodes, *queue_nodes, *loop_entries; /* used in building loop tree */
  int max_int_hard_regs, max_fp_hard_regs;
};

static inline struct gen_ctx **gen_ctx_loc (MIR_context_t ctx) { return (struct gen_ctx **) ctx; }

#define optimize_level gen_ctx->optimize_level
#define curr_func_item gen_ctx->curr_func_item
#define debug_file gen_ctx->debug_file
#define insn_to_consider gen_ctx->insn_to_consider
#define temp_bitmap gen_ctx->temp_bitmap
#define temp_bitmap2 gen_ctx->temp_bitmap2
#define all_vars gen_ctx->all_vars
#define call_used_hard_regs gen_ctx->call_used_hard_regs
#define curr_cfg gen_ctx->curr_cfg
#define curr_bb_index gen_ctx->curr_bb_index
#define curr_loop_node_index gen_ctx->curr_loop_node_index
#define loop_nodes gen_ctx->loop_nodes
#define queue_nodes gen_ctx->queue_nodes
#define loop_entries gen_ctx->loop_entries
#define max_int_hard_regs gen_ctx->max_int_hard_regs
#define max_fp_hard_regs gen_ctx->max_fp_hard_regs

#ifdef __x86_64__
#include "mir-gen-x86_64.c"
#elif defined(__aarch64__)
#include "mir-gen-aarch64.c"
#elif defined(__PPC64__)
#include "mir-gen-ppc64.c"
#else
#error "undefined or unsupported generation target"
#endif

#define DEFAULT_INIT_BITMAP_BITS_NUM 256

static void make_io_dup_op_insns (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_func_t func;
  MIR_insn_t insn, next_insn;
  MIR_insn_code_t code;
  MIR_op_t input, output, temp_op;
  MIR_op_mode_t mode;
  MIR_type_t type;
  size_t i;
  int out_p;

  gen_assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  for (i = 0; target_io_dup_op_insn_codes[i] != MIR_INSN_BOUND; i++)
    bitmap_set_bit_p (insn_to_consider, target_io_dup_op_insn_codes[i]);
  if (bitmap_empty_p (insn_to_consider)) return;
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = next_insn) {
    next_insn = DLIST_NEXT (MIR_insn_t, insn);
    code = insn->code;
    if (!bitmap_bit_p (insn_to_consider, code)) continue;
    gen_assert (MIR_insn_nops (ctx, insn) >= 2 && !MIR_call_code_p (code) && code != MIR_RET);
    mode = MIR_insn_op_mode (ctx, insn, 0, &out_p);
    gen_assert (out_p && mode == MIR_insn_op_mode (ctx, insn, 1, &out_p) && !out_p);
    output = insn->ops[0];
    input = insn->ops[1];
    gen_assert (input.mode == MIR_OP_REG || input.mode == MIR_OP_HARD_REG
                || output.mode == MIR_OP_REG || output.mode == MIR_OP_HARD_REG);
    if (input.mode == output.mode
        && ((input.mode == MIR_OP_HARD_REG && input.u.hard_reg == output.u.hard_reg)
            || (input.mode == MIR_OP_REG && input.u.reg == output.u.reg)))
      continue;
    if (mode == MIR_OP_FLOAT) {
      code = MIR_FMOV;
      type = MIR_T_F;
    } else if (mode == MIR_OP_DOUBLE) {
      code = MIR_DMOV;
      type = MIR_T_D;
    } else if (mode == MIR_OP_LDOUBLE) {
      code = MIR_LDMOV;
      type = MIR_T_LD;
    } else {
      code = MIR_MOV;
      type = MIR_T_I64;
    }
    temp_op = MIR_new_reg_op (ctx, gen_new_temp_reg (ctx, type, func));
    gen_add_insn_before (ctx, insn, MIR_new_insn (ctx, code, temp_op, insn->ops[1]));
    gen_add_insn_after (ctx, insn, MIR_new_insn (ctx, code, insn->ops[0], temp_op));
    insn->ops[0] = insn->ops[1] = temp_op;
  }
}

typedef struct dead_var *dead_var_t;

DEF_DLIST_LINK (dead_var_t);

typedef struct bb *bb_t;

DEF_DLIST_LINK (bb_t);

typedef struct bb_insn *bb_insn_t;

DEF_DLIST_LINK (bb_insn_t);

typedef struct edge *edge_t;

typedef edge_t in_edge_t;

typedef edge_t out_edge_t;

DEF_DLIST_LINK (in_edge_t);
DEF_DLIST_LINK (out_edge_t);

struct edge {
  bb_t src, dst;
  DLIST_LINK (in_edge_t) in_link;
  DLIST_LINK (out_edge_t) out_link;
  unsigned char back_edge_p;
  unsigned char skipped_p; /* used for CCP */
};

DEF_DLIST (in_edge_t, in_link);
DEF_DLIST (out_edge_t, out_link);

struct dead_var {
  MIR_reg_t var;
  DLIST_LINK (dead_var_t) dead_var_link;
};

DEF_DLIST (dead_var_t, dead_var_link);

struct bb_insn {
  MIR_insn_t insn;
  unsigned char flag, flag2; /* used for CCP and LICM */
  size_t index;              /* used for LICM */
  DLIST_LINK (bb_insn_t) bb_insn_link;
  bb_t bb;
  DLIST (dead_var_t) dead_vars;
  bitmap_t call_hard_reg_args; /* non-null for calls */
  size_t label_disp;           /* for label */
};

DEF_DLIST (bb_insn_t, bb_insn_link);

struct bb {
  size_t index, pre, rpost, bfs; /* preorder, reverse post order, breadth first order */
  unsigned int flag;             /* used for CCP */
  DLIST_LINK (bb_t) bb_link;
  DLIST (in_edge_t) in_edges;
  /* The out edges order: optional fall through bb, optional label bb,
     optional exit bb.  There is always at least one edge.  */
  DLIST (out_edge_t) out_edges;
  DLIST (bb_insn_t) bb_insns;
  size_t freq;
  bitmap_t in, out, gen, kill; /* var bitmaps for different data flow problems */
  bitmap_t dom_in, dom_out;    /* additional var bitmaps LICM */
  loop_node_t loop_node;
  int max_int_pressure, max_fp_pressure;
};

DEF_DLIST (bb_t, bb_link);

DEF_DLIST_LINK (loop_node_t);
DEF_DLIST_TYPE (loop_node_t);

struct loop_node {
  size_t index; /* if BB != NULL, it is index of BB */
  bb_t bb;      /* NULL for internal tree node  */
  loop_node_t entry;
  loop_node_t parent;
  bb_t preheader;      /* used in LICM */
  unsigned int call_p; /* used in LICM: call is present in the loop */
  DLIST (loop_node_t) children;
  DLIST_LINK (loop_node_t) children_link;
  int max_int_pressure, max_fp_pressure;
};

DEF_DLIST_CODE (loop_node_t, children_link);

DEF_DLIST_LINK (func_cfg_t);

typedef struct mv *mv_t;
typedef mv_t dst_mv_t;
typedef mv_t src_mv_t;

DEF_DLIST_LINK (mv_t);
DEF_DLIST_LINK (dst_mv_t);
DEF_DLIST_LINK (src_mv_t);

struct mv {
  bb_insn_t bb_insn;
  size_t freq;
  DLIST_LINK (mv_t) mv_link;
  DLIST_LINK (dst_mv_t) dst_link;
  DLIST_LINK (src_mv_t) src_link;
};

DEF_DLIST (mv_t, mv_link);
DEF_DLIST (dst_mv_t, dst_link);
DEF_DLIST (src_mv_t, src_link);

struct reg_info {
  long freq;
  size_t calls_num;
  /* The followd members are defined and used only in RA */
  long thread_freq; /* thread accumulated freq, defined for first thread breg */
  /* first/next breg of the same thread, MIR_MAX_REG_NUM is end mark  */
  MIR_reg_t thread_first, thread_next;
  size_t live_length; /* # of program points where breg lives */
  DLIST (dst_mv_t) dst_moves;
  DLIST (src_mv_t) src_moves;
};

typedef struct reg_info reg_info_t;

DEF_VARR (reg_info_t);

typedef struct {
  int uns_p;
  union {
    int64_t i;
    uint64_t u;
  } u;
} const_t;

#if !MIR_NO_GEN_DEBUG
static void print_const (FILE *f, const_t c) {
  if (c.uns_p)
    fprintf (f, "%" PRIu64, c.u.u);
  else
    fprintf (f, "%" PRId64, c.u.i);
}
#endif

struct func_cfg {
  MIR_reg_t min_reg, max_reg;
  size_t non_conflicting_moves;  /* # of moves with non-conflicting regs */
  VARR (reg_info_t) * breg_info; /* bregs */
  DLIST (bb_t) bbs;
  DLIST (mv_t) used_moves, free_moves;
  loop_node_t root_loop_node;
};

static DLIST (dead_var_t) free_dead_vars;

static void init_dead_vars (void) { DLIST_INIT (dead_var_t, free_dead_vars); }

static void free_dead_var (dead_var_t dv) { DLIST_APPEND (dead_var_t, free_dead_vars, dv); }

static dead_var_t get_dead_var (MIR_context_t ctx) {
  dead_var_t dv;

  if ((dv = DLIST_HEAD (dead_var_t, free_dead_vars)) == NULL)
    return gen_malloc (ctx, sizeof (struct dead_var));
  DLIST_REMOVE (dead_var_t, free_dead_vars, dv);
  return dv;
}

static void finish_dead_vars (void) {
  dead_var_t dv;

  while ((dv = DLIST_HEAD (dead_var_t, free_dead_vars)) != NULL) {
    DLIST_REMOVE (dead_var_t, free_dead_vars, dv);
    free (dv);
  }
}

static void add_bb_insn_dead_var (MIR_context_t ctx, bb_insn_t bb_insn, MIR_reg_t var) {
  dead_var_t dv;

  for (dv = DLIST_HEAD (dead_var_t, bb_insn->dead_vars); dv != NULL;
       dv = DLIST_NEXT (dead_var_t, dv))
    if (dv->var == var) return;
  dv = get_dead_var (ctx);
  dv->var = var;
  DLIST_APPEND (dead_var_t, bb_insn->dead_vars, dv);
}

static dead_var_t find_bb_insn_dead_var (bb_insn_t bb_insn, MIR_reg_t var) {
  dead_var_t dv;

  for (dv = DLIST_HEAD (dead_var_t, bb_insn->dead_vars); dv != NULL;
       dv = DLIST_NEXT (dead_var_t, dv))
    if (dv->var == var) return dv;
  return NULL;
}

static void clear_bb_insn_dead_vars (bb_insn_t bb_insn) {
  dead_var_t dv;

  while ((dv = DLIST_HEAD (dead_var_t, bb_insn->dead_vars)) != NULL) {
    DLIST_REMOVE (dead_var_t, bb_insn->dead_vars, dv);
    free_dead_var (dv);
  }
}

static void remove_bb_insn_dead_var (bb_insn_t bb_insn, MIR_reg_t hr) {
  dead_var_t dv, next_dv;

  gen_assert (hr <= MAX_HARD_REG);
  for (dv = DLIST_HEAD (dead_var_t, bb_insn->dead_vars); dv != NULL; dv = next_dv) {
    next_dv = DLIST_NEXT (dead_var_t, dv);
    if (dv->var != hr) continue;
    DLIST_REMOVE (dead_var_t, bb_insn->dead_vars, dv);
    free_dead_var (dv);
  }
}

static void move_bb_insn_dead_vars (bb_insn_t bb_insn, bb_insn_t from_bb_insn) {
  dead_var_t dv;

  while ((dv = DLIST_HEAD (dead_var_t, from_bb_insn->dead_vars)) != NULL) {
    DLIST_REMOVE (dead_var_t, from_bb_insn->dead_vars, dv);
    DLIST_APPEND (dead_var_t, bb_insn->dead_vars, dv);
  }
}

static bb_insn_t create_bb_insn (MIR_context_t ctx, MIR_insn_t insn, bb_t bb) {
  bb_insn_t bb_insn = gen_malloc (ctx, sizeof (struct bb_insn));

  insn->data = bb_insn;
  bb_insn->bb = bb;
  bb_insn->insn = insn;
  bb_insn->flag = FALSE;
  bb_insn->call_hard_reg_args = NULL;
  DLIST_INIT (dead_var_t, bb_insn->dead_vars);
  if (MIR_call_code_p (insn->code)) bb_insn->call_hard_reg_args = bitmap_create2 (MAX_HARD_REG + 1);
  return bb_insn;
}

static bb_insn_t add_new_bb_insn (MIR_context_t ctx, MIR_insn_t insn, bb_t bb) {
  bb_insn_t bb_insn = create_bb_insn (ctx, insn, bb);

  DLIST_APPEND (bb_insn_t, bb->bb_insns, bb_insn);
  return bb_insn;
}

static void delete_bb_insn (bb_insn_t bb_insn) {
  DLIST_REMOVE (bb_insn_t, bb_insn->bb->bb_insns, bb_insn);
  bb_insn->insn->data = NULL;
  clear_bb_insn_dead_vars (bb_insn);
  if (bb_insn->call_hard_reg_args != NULL) bitmap_destroy (bb_insn->call_hard_reg_args);
  free (bb_insn);
}

static void create_new_bb_insns (MIR_context_t ctx, MIR_insn_t before, MIR_insn_t after,
                                 MIR_insn_t insn_for_bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn;
  bb_insn_t bb_insn, new_bb_insn;
  bb_t bb;

  if (insn_for_bb == NULL)                  /* It should be in the 1st block */
    bb = DLIST_EL (bb_t, curr_cfg->bbs, 2); /* Skip entry and exit blocks */
  else
    bb = ((bb_insn_t) insn_for_bb->data)->bb;
  if (before != NULL && (bb_insn = before->data)->bb == bb) {
    for (insn = DLIST_NEXT (MIR_insn_t, before); insn != after;
         insn = DLIST_NEXT (MIR_insn_t, insn), bb_insn = new_bb_insn) {
      new_bb_insn = create_bb_insn (ctx, insn, bb);
      DLIST_INSERT_AFTER (bb_insn_t, bb->bb_insns, bb_insn, new_bb_insn);
    }
  } else {
    gen_assert (after != NULL);
    bb_insn = after->data;
    insn = (before == NULL ? DLIST_HEAD (MIR_insn_t, curr_func_item->u.func->insns)
                           : DLIST_NEXT (MIR_insn_t, before));
    for (; insn != after; insn = DLIST_NEXT (MIR_insn_t, insn)) {
      new_bb_insn = create_bb_insn (ctx, insn, bb);
      if (bb == bb_insn->bb)
        DLIST_INSERT_BEFORE (bb_insn_t, bb->bb_insns, bb_insn, new_bb_insn);
      else
        DLIST_APPEND (bb_insn_t, bb->bb_insns, new_bb_insn);
    }
  }
}

static void gen_delete_insn (MIR_context_t ctx, MIR_insn_t insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  delete_bb_insn (insn->data);
  MIR_remove_insn (ctx, curr_func_item, insn);
}

static void gen_add_insn_before (MIR_context_t ctx, MIR_insn_t before, MIR_insn_t insn) {
  MIR_insn_t insn_for_bb = before;
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_assert (!MIR_branch_code_p (insn->code) && insn->code != MIR_LABEL);
  if (before->code == MIR_LABEL) {
    insn_for_bb = DLIST_PREV (MIR_insn_t, before);
    gen_assert (insn_for_bb == NULL || !MIR_branch_code_p (insn_for_bb->code));
  }
  MIR_insert_insn_before (ctx, curr_func_item, before, insn);
  create_new_bb_insns (ctx, DLIST_PREV (MIR_insn_t, insn), before, insn_for_bb);
}

static void gen_add_insn_after (MIR_context_t ctx, MIR_insn_t after, MIR_insn_t insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_assert (insn->code != MIR_LABEL);
  gen_assert (!MIR_branch_code_p (after->code));
  MIR_insert_insn_after (ctx, curr_func_item, after, insn);
  create_new_bb_insns (ctx, after, DLIST_NEXT (MIR_insn_t, insn), after);
}

static void setup_call_hard_reg_args (MIR_insn_t call_insn, MIR_reg_t hard_reg) {
  bb_insn_t bb_insn = call_insn->data;

  gen_assert (MIR_call_code_p (call_insn->code) && hard_reg <= MAX_HARD_REG);
  bitmap_set_bit_p (bb_insn->call_hard_reg_args, hard_reg);
}

static void set_label_disp (MIR_insn_t insn, size_t disp) {
  gen_assert (insn->code == MIR_LABEL);
  ((bb_insn_t) insn->data)->label_disp = disp;
}
static size_t get_label_disp (MIR_insn_t insn) {
  gen_assert (insn->code == MIR_LABEL);
  return ((bb_insn_t) insn->data)->label_disp;
}

static bb_t create_bb (MIR_context_t ctx, MIR_insn_t insn) {
  bb_t bb = gen_malloc (ctx, sizeof (struct bb));

  bb->pre = bb->rpost = bb->bfs = 0;
  bb->flag = FALSE;
  bb->loop_node = NULL;
  DLIST_INIT (bb_insn_t, bb->bb_insns);
  DLIST_INIT (in_edge_t, bb->in_edges);
  DLIST_INIT (out_edge_t, bb->out_edges);
  bb->in = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  bb->out = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  bb->gen = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  bb->kill = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  bb->dom_in = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  bb->dom_out = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  bb->max_int_pressure = bb->max_fp_pressure = 0;
  if (insn != NULL) add_new_bb_insn (ctx, insn, bb);
  return bb;
}

static void add_bb (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  DLIST_APPEND (bb_t, curr_cfg->bbs, bb);
  bb->index = curr_bb_index++;
}

static edge_t create_edge (MIR_context_t ctx, bb_t src, bb_t dst, int append_p) {
  edge_t e = gen_malloc (ctx, sizeof (struct edge));

  e->src = src;
  e->dst = dst;
  if (append_p) {
    DLIST_APPEND (in_edge_t, dst->in_edges, e);
    DLIST_APPEND (out_edge_t, src->out_edges, e);
  } else {
    DLIST_PREPEND (in_edge_t, dst->in_edges, e);
    DLIST_PREPEND (out_edge_t, src->out_edges, e);
  }
  e->back_edge_p = e->skipped_p = FALSE;
  return e;
}

static void delete_edge (edge_t e) {
  DLIST_REMOVE (out_edge_t, e->src->out_edges, e);
  DLIST_REMOVE (in_edge_t, e->dst->in_edges, e);
  free (e);
}

static void delete_bb (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  edge_t e, next_e;

  for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = next_e) {
    next_e = DLIST_NEXT (out_edge_t, e);
    delete_edge (e);
  }
  for (e = DLIST_HEAD (in_edge_t, bb->in_edges); e != NULL; e = next_e) {
    next_e = DLIST_NEXT (in_edge_t, e);
    delete_edge (e);
  }
  DLIST_REMOVE (bb_t, curr_cfg->bbs, bb);
  bitmap_destroy (bb->in);
  bitmap_destroy (bb->out);
  bitmap_destroy (bb->gen);
  bitmap_destroy (bb->kill);
  bitmap_destroy (bb->dom_in);
  bitmap_destroy (bb->dom_out);
  free (bb);
}

static void DFS (bb_t bb, size_t *pre, size_t *rpost) {
  edge_t e;

  bb->pre = (*pre)++;
  for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_NEXT (out_edge_t, e))
    if (e->dst->pre == 0)
      DFS (e->dst, pre, rpost);
    else if (e->dst->rpost == 0)
      e->back_edge_p = TRUE;
  bb->rpost = (*rpost)--;
}

static void enumerate_bbs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t pre, rpost;

  pre = 1;
  rpost = DLIST_LENGTH (bb_t, curr_cfg->bbs);
  DFS (DLIST_HEAD (bb_t, curr_cfg->bbs), &pre, &rpost);
}

static loop_node_t top_loop_node (bb_t bb) {
  for (loop_node_t loop_node = bb->loop_node;; loop_node = loop_node->parent)
    if (loop_node->parent == NULL) return loop_node;
}

static int in_loop_p (loop_node_t node, loop_node_t loop) {
  for (loop_node_t curr_node = node; curr_node != NULL; curr_node = curr_node->parent)
    if (curr_node == loop) return TRUE;
  return FALSE;
}

static int bb_loop_exit_p (MIR_context_t ctx, bb_t bb, loop_node_t loop) {
  for (edge_t e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_PREV (out_edge_t, e))
    if (!in_loop_p (e->dst->loop_node, loop)) return TRUE;
  return FALSE;
}

static loop_node_t create_loop_node (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  loop_node_t loop_node = gen_malloc (ctx, sizeof (struct loop_node));

  loop_node->index = curr_loop_node_index++;
  loop_node->bb = bb;
  if (bb != NULL) bb->loop_node = loop_node;
  loop_node->parent = NULL;
  loop_node->entry = NULL;
  loop_node->preheader = NULL;
  loop_node->max_int_pressure = loop_node->max_fp_pressure = 0;
  DLIST_INIT (loop_node_t, loop_node->children);
  return loop_node;
}

static int process_loop (MIR_context_t ctx, bb_t entry_bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  edge_t e;
  loop_node_t loop_node, new_loop_node, queue_node;
  bb_t queue_bb;

  VARR_TRUNC (loop_node_t, loop_nodes, 0);
  VARR_TRUNC (loop_node_t, queue_nodes, 0);
  bitmap_clear (temp_bitmap);
  for (e = DLIST_HEAD (in_edge_t, entry_bb->in_edges); e != NULL; e = DLIST_NEXT (in_edge_t, e))
    if (e->back_edge_p && e->src != entry_bb) {
      loop_node = top_loop_node (e->src);
      if (!bitmap_set_bit_p (temp_bitmap, loop_node->index)) continue;
      VARR_PUSH (loop_node_t, loop_nodes, loop_node);
      VARR_PUSH (loop_node_t, queue_nodes, loop_node);
    }
  while (VARR_LENGTH (loop_node_t, queue_nodes) != 0) {
    queue_node = VARR_POP (loop_node_t, queue_nodes);
    if ((queue_bb = queue_node->bb) == NULL) queue_bb = queue_node->entry->bb; /* subloop */
    /* entry block is achieved which means multiple entry loop -- just ignore */
    if (queue_bb == DLIST_HEAD (bb_t, curr_cfg->bbs)) return FALSE;
    for (e = DLIST_HEAD (in_edge_t, queue_bb->in_edges); e != NULL; e = DLIST_NEXT (in_edge_t, e))
      if (e->src != entry_bb) {
        loop_node = top_loop_node (e->src);
        if (!bitmap_set_bit_p (temp_bitmap, loop_node->index)) continue;
        VARR_PUSH (loop_node_t, loop_nodes, loop_node);
        VARR_PUSH (loop_node_t, queue_nodes, loop_node);
      }
  }
  loop_node = entry_bb->loop_node;
  VARR_PUSH (loop_node_t, loop_nodes, loop_node);
  new_loop_node = create_loop_node (ctx, NULL);
  new_loop_node->entry = loop_node;
  while (VARR_LENGTH (loop_node_t, loop_nodes) != 0) {
    loop_node = VARR_POP (loop_node_t, loop_nodes);
    DLIST_APPEND (loop_node_t, new_loop_node->children, loop_node);
    loop_node->parent = new_loop_node;
  }
  return TRUE;
}

static void setup_loop_pressure (MIR_context_t ctx, loop_node_t loop_node) {
  for (loop_node_t curr = DLIST_HEAD (loop_node_t, loop_node->children); curr != NULL;
       curr = DLIST_NEXT (loop_node_t, curr)) {
    if (curr->bb == NULL) {
      setup_loop_pressure (ctx, curr);
    } else {
      curr->max_int_pressure = curr->bb->max_int_pressure;
      curr->max_fp_pressure = curr->bb->max_fp_pressure;
    }
    if (loop_node->max_int_pressure < curr->max_int_pressure)
      loop_node->max_int_pressure = curr->max_int_pressure;
    if (loop_node->max_fp_pressure < curr->max_fp_pressure)
      loop_node->max_fp_pressure = curr->max_fp_pressure;
  }
}

static int compare_bb_loop_nodes (const void *p1, const void *p2) {
  bb_t bb1 = (*(const loop_node_t *) p1)->bb, bb2 = (*(const loop_node_t *) p2)->bb;

  return bb1->rpost > bb2->rpost ? -1 : bb1->rpost < bb2->rpost ? 1 : 0;
}

static int build_loop_tree (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  loop_node_t loop_node;
  edge_t e;
  int loops_p = FALSE;

  curr_loop_node_index = 0;
  enumerate_bbs (ctx);
  VARR_TRUNC (loop_node_t, loop_entries, 0);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    loop_node = create_loop_node (ctx, bb);
    loop_node->entry = loop_node;
    for (e = DLIST_HEAD (in_edge_t, bb->in_edges); e != NULL; e = DLIST_NEXT (in_edge_t, e))
      if (e->back_edge_p) {
        VARR_PUSH (loop_node_t, loop_entries, loop_node);
        break;
      }
  }
  qsort (VARR_ADDR (loop_node_t, loop_entries), VARR_LENGTH (loop_node_t, loop_entries),
         sizeof (loop_node_t), compare_bb_loop_nodes);
  for (size_t i = 0; i < VARR_LENGTH (loop_node_t, loop_entries); i++)
    if (process_loop (ctx, VARR_GET (loop_node_t, loop_entries, i)->bb)) loops_p = TRUE;
  curr_cfg->root_loop_node = create_loop_node (ctx, NULL);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb))
    if ((loop_node = top_loop_node (bb)) != curr_cfg->root_loop_node) {
      DLIST_APPEND (loop_node_t, curr_cfg->root_loop_node->children, loop_node);
      loop_node->parent = curr_cfg->root_loop_node;
    }
  setup_loop_pressure (ctx, curr_cfg->root_loop_node);
  return loops_p;
}

static void destroy_loop_tree (MIR_context_t ctx, loop_node_t root) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  loop_node_t node, next;

  if (root->bb != NULL) {
    root->bb->loop_node = NULL;
  } else {
    for (node = DLIST_HEAD (loop_node_t, root->children); node != NULL; node = next) {
      next = DLIST_NEXT (loop_node_t, node);
      destroy_loop_tree (ctx, node);
    }
  }
  free (root);
}

static void update_min_max_reg (MIR_context_t ctx, MIR_reg_t reg) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (reg == 0) return;
  if (curr_cfg->max_reg == 0 || curr_cfg->min_reg > reg) curr_cfg->min_reg = reg;
  if (curr_cfg->max_reg < reg) curr_cfg->max_reg = reg;
}

static MIR_reg_t gen_new_temp_reg (MIR_context_t ctx, MIR_type_t type, MIR_func_t func) {
  MIR_reg_t reg = _MIR_new_temp_reg (ctx, type, func);

  update_min_max_reg (ctx, reg);
  return reg;
}

static MIR_reg_t reg2breg (struct gen_ctx *gen_ctx, MIR_reg_t reg) {
  return reg - curr_cfg->min_reg;
}
static MIR_reg_t breg2reg (struct gen_ctx *gen_ctx, MIR_reg_t breg) {
  return breg + curr_cfg->min_reg;
}
static MIR_reg_t reg2var (struct gen_ctx *gen_ctx, MIR_reg_t reg) {
  return reg2breg (gen_ctx, reg) + MAX_HARD_REG + 1;
}
static MIR_reg_t var_is_reg_p (MIR_reg_t var) { return var > MAX_HARD_REG; }
static MIR_reg_t var2reg (struct gen_ctx *gen_ctx, MIR_reg_t var) {
  gen_assert (var > MAX_HARD_REG);
  return breg2reg (gen_ctx, var - MAX_HARD_REG - 1);
}
static MIR_reg_t var2breg (struct gen_ctx *gen_ctx, MIR_reg_t var) {
  gen_assert (var > MAX_HARD_REG);
  return var - MAX_HARD_REG - 1;
}

static MIR_reg_t get_nregs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  return curr_cfg->max_reg == 0 ? 0 : curr_cfg->max_reg - curr_cfg->min_reg + 1;
}

static MIR_reg_t get_nvars (MIR_context_t ctx) { return get_nregs (ctx) + MAX_HARD_REG + 1; }

static int move_p (MIR_insn_t insn) {
  return ((insn->code == MIR_MOV || insn->code == MIR_FMOV || insn->code == MIR_DMOV
           || insn->code == MIR_LDMOV)
          && (insn->ops[0].mode == MIR_OP_REG || insn->ops[0].mode == MIR_OP_HARD_REG)
          && (insn->ops[1].mode == MIR_OP_REG || insn->ops[1].mode == MIR_OP_HARD_REG));
}

static int imm_move_p (MIR_insn_t insn) {
  return ((insn->code == MIR_MOV || insn->code == MIR_FMOV || insn->code == MIR_DMOV
           || insn->code == MIR_LDMOV)
          && (insn->ops[0].mode == MIR_OP_REG || insn->ops[0].mode == MIR_OP_HARD_REG)
          && (insn->ops[1].mode == MIR_OP_INT || insn->ops[1].mode == MIR_OP_UINT
              || insn->ops[1].mode == MIR_OP_FLOAT || insn->ops[1].mode == MIR_OP_DOUBLE
              || insn->ops[1].mode == MIR_OP_LDOUBLE || insn->ops[1].mode == MIR_OP_REF));
}

typedef struct {
  MIR_insn_t insn;
  size_t nops, op_num, op_part_num, passed_mem_num;
} insn_var_iterator_t;

static inline void insn_var_iterator_init (MIR_context_t ctx, insn_var_iterator_t *iter,
                                           MIR_insn_t insn) {
  iter->insn = insn;
  iter->nops = MIR_insn_nops (ctx, insn);
  iter->op_num = 0;
  iter->op_part_num = 0;
  iter->passed_mem_num = 0;
}

static inline int insn_var_iterator_next (MIR_context_t ctx, insn_var_iterator_t *iter,
                                          MIR_reg_t *var, int *out_p, int *mem_p,
                                          size_t *passed_mem_num) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_op_t op;

  while (iter->op_num < iter->nops) {
    MIR_insn_op_mode (ctx, iter->insn, iter->op_num, out_p);
    op = iter->insn->ops[iter->op_num];
    *mem_p = FALSE;
    *passed_mem_num = iter->passed_mem_num;
    while (iter->op_part_num < 2) {
      if (op.mode == MIR_OP_MEM || op.mode == MIR_OP_HARD_REG_MEM) {
        *mem_p = TRUE;
        *passed_mem_num = ++iter->passed_mem_num;
        *out_p = FALSE;
        if (op.mode == MIR_OP_MEM) {
          *var = iter->op_part_num == 0 ? op.u.mem.base : op.u.mem.index;
          if (*var == 0) {
            iter->op_part_num++;
            continue;
          }
          *var = reg2var (gen_ctx, *var);
        } else {
          *var = iter->op_part_num == 0 ? op.u.hard_reg_mem.base : op.u.hard_reg_mem.index;
          if (*var == MIR_NON_HARD_REG) {
            iter->op_part_num++;
            continue;
          }
        }
      } else if (iter->op_part_num > 0) {
        break;
      } else if (op.mode == MIR_OP_REG) {
        *var = reg2var (gen_ctx, op.u.reg);
      } else if (op.mode == MIR_OP_HARD_REG) {
        *var = op.u.hard_reg;
      } else
        break;
      iter->op_part_num++;
      return TRUE;
    }
    iter->op_num++;
    iter->op_part_num = 0;
  }
  return FALSE;
}

#define FOREACH_INSN_VAR(ctx, iterator, insn, var, out_p, mem_p, passed_mem_num) \
  for (insn_var_iterator_init (ctx, &iterator, insn);                            \
       insn_var_iterator_next (ctx, &iterator, &var, &out_p, &mem_p, &passed_mem_num);)

#if !MIR_NO_GEN_DEBUG
static void output_in_edges (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  edge_t e;

  fprintf (debug_file, "  in edges:");
  for (e = DLIST_HEAD (in_edge_t, bb->in_edges); e != NULL; e = DLIST_NEXT (in_edge_t, e)) {
    fprintf (debug_file, " %3lu", (unsigned long) e->src->index);
    if (e->skipped_p) fprintf (debug_file, "(CCP skip)");
  }
  fprintf (debug_file, "\n");
}

static void output_out_edges (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  edge_t e;

  fprintf (debug_file, "  out edges:");
  for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_NEXT (out_edge_t, e)) {
    fprintf (debug_file, " %3lu", (unsigned long) e->dst->index);
    if (e->skipped_p) fprintf (debug_file, "(CCP skip)");
  }
  fprintf (debug_file, "\n");
}

static void output_bitmap (MIR_context_t ctx, const char *head, bitmap_t bm, int var_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t nel;
  bitmap_iterator_t bi;

  if (bm == NULL || bitmap_empty_p (bm)) return;
  fprintf (debug_file, "%s", head);
  FOREACH_BITMAP_BIT (bi, bm, nel) {
    fprintf (debug_file, " %3lu", (unsigned long) nel);
    if (var_p && var_is_reg_p (nel))
      fprintf (debug_file, "(%s:%s)",
               MIR_type_str (ctx,
                             MIR_reg_type (ctx, var2reg (gen_ctx, nel), curr_func_item->u.func)),
               MIR_reg_name (ctx, var2reg (gen_ctx, nel), curr_func_item->u.func));
  }
  fprintf (debug_file, "\n");
}

static void print_bb_insn (MIR_context_t ctx, bb_insn_t bb_insn, int with_notes_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_op_t op;

  MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, FALSE);
  if (with_notes_p) {
    for (dead_var_t dv = DLIST_HEAD (dead_var_t, bb_insn->dead_vars); dv != NULL;
         dv = DLIST_NEXT (dead_var_t, dv)) {
      if (var_is_reg_p (dv->var)) {
        op.mode = MIR_OP_REG;
        op.u.reg = var2reg (gen_ctx, dv->var);
      } else {
        op.mode = MIR_OP_HARD_REG;
        op.u.hard_reg = dv->var;
      }
      fprintf (debug_file, dv == DLIST_HEAD (dead_var_t, bb_insn->dead_vars) ? " # dead: " : " ");
      MIR_output_op (ctx, debug_file, op, curr_func_item->u.func);
    }
  }
  fprintf (debug_file, "\n");
}

static void print_CFG (MIR_context_t ctx, int bb_p, int pressure_p, int insns_p, int insn_index_p,
                       void (*bb_info_print_func) (MIR_context_t, bb_t)) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    if (bb_p) {
      fprintf (debug_file, "BB %3lu", (unsigned long) bb->index);
      if (pressure_p)
        fprintf (debug_file, " (pressure: int=%d, fp=%d)", bb->max_int_pressure,
                 bb->max_fp_pressure);
      if (bb->loop_node == NULL)
        fprintf (debug_file, "\n");
      else
        fprintf (debug_file, " (loop%3lu):\n", (unsigned long) bb->loop_node->parent->index);
      output_in_edges (ctx, bb);
      output_out_edges (ctx, bb);
      if (bb_info_print_func != NULL) {
        bb_info_print_func (ctx, bb);
        fprintf (debug_file, "\n");
      }
    }
    if (insns_p) {
      for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
           bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
        if (insn_index_p) fprintf (debug_file, "  %-5lu", (unsigned long) bb_insn->index);
        print_bb_insn (ctx, bb_insn, TRUE);
      }
      fprintf (debug_file, "\n");
    }
  }
}

static void print_loop_subtree (MIR_context_t ctx, loop_node_t root, int level, int bb_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (root->bb != NULL && !bb_p) return;
  for (int i = 0; i < 2 * level + 2; i++) fprintf (debug_file, " ");
  if (root->bb != NULL) {
    gen_assert (DLIST_HEAD (loop_node_t, root->children) == NULL);
    fprintf (debug_file, "BB%-3lu (pressure: int=%d, fp=%d)\n", (unsigned long) root->bb->index,
             root->max_int_pressure, root->max_fp_pressure);
    return;
  }
  fprintf (debug_file, "Loop%3lu (pressure: int=%d, fp=%d)", (unsigned long) root->index,
           root->max_int_pressure, root->max_fp_pressure);
  if (curr_cfg->root_loop_node == root)
    fprintf (debug_file, ":\n");
  else
    fprintf (debug_file, " (entry - bb%lu):\n", (unsigned long) root->entry->bb->index);
  for (loop_node_t node = DLIST_HEAD (loop_node_t, root->children); node != NULL;
       node = DLIST_NEXT (loop_node_t, node))
    print_loop_subtree (ctx, node, level + 1, bb_p);
}

static void print_loop_tree (MIR_context_t ctx, int bb_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  fprintf (debug_file, "Loop Tree\n");
  print_loop_subtree (ctx, curr_cfg->root_loop_node, 0, bb_p);
}

#endif

static mv_t get_free_move (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  mv_t mv;

  if ((mv = DLIST_HEAD (mv_t, curr_cfg->free_moves)) != NULL)
    DLIST_REMOVE (mv_t, curr_cfg->free_moves, mv);
  else
    mv = gen_malloc (ctx, sizeof (struct mv));
  DLIST_APPEND (mv_t, curr_cfg->used_moves, mv);
  return mv;
}

static void free_move (MIR_context_t ctx, mv_t mv) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  DLIST_REMOVE (mv_t, curr_cfg->used_moves, mv);
  DLIST_APPEND (mv_t, curr_cfg->free_moves, mv);
}

static void build_func_cfg (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn, next_insn;
  bb_insn_t bb_insn, label_bb_insn;
  size_t i, nops;
  MIR_op_t *op;
  MIR_var_t var;
  bb_t bb, prev_bb, entry_bb, exit_bb;

  DLIST_INIT (bb_t, curr_cfg->bbs);
  DLIST_INIT (mv_t, curr_cfg->used_moves);
  DLIST_INIT (mv_t, curr_cfg->free_moves);
  curr_cfg->max_reg = 0;
  curr_cfg->min_reg = 0;
  curr_cfg->root_loop_node = NULL;
  curr_bb_index = 0;
  for (i = 0; i < VARR_LENGTH (MIR_var_t, curr_func_item->u.func->vars); i++) {
    var = VARR_GET (MIR_var_t, curr_func_item->u.func->vars, i);
    update_min_max_reg (ctx, MIR_reg (ctx, var.name, curr_func_item->u.func));
  }
  entry_bb = create_bb (ctx, NULL);
  add_bb (ctx, entry_bb);
  exit_bb = create_bb (ctx, NULL);
  add_bb (ctx, exit_bb);
  insn = DLIST_HEAD (MIR_insn_t, curr_func_item->u.func->insns);
  if (insn != NULL) {
    bb = create_bb (ctx, NULL);
    add_bb (ctx, bb);
    if (insn->code == MIR_LABEL) { /* Create one more BB.  First BB will be empty. */
      prev_bb = bb;
      bb = create_bb (ctx, NULL);
      add_bb (ctx, bb);
      create_edge (ctx, prev_bb, bb, TRUE);
    }
  }
  for (; insn != NULL; insn = next_insn) {
    next_insn = DLIST_NEXT (MIR_insn_t, insn);
    if (insn->data == NULL) add_new_bb_insn (ctx, insn, bb);
    nops = MIR_insn_nops (ctx, insn);
    if (next_insn != NULL
        && (MIR_branch_code_p (insn->code) || insn->code == MIR_RET || insn->code == MIR_SWITCH
            || next_insn->code == MIR_LABEL)) {
      prev_bb = bb;
      if (next_insn->code == MIR_LABEL && (label_bb_insn = next_insn->data) != NULL)
        bb = label_bb_insn->bb;
      else
        bb = create_bb (ctx, next_insn);
      add_bb (ctx, bb);
      if (insn->code != MIR_JMP && insn->code != MIR_RET && insn->code != MIR_SWITCH)
        create_edge (ctx, prev_bb, bb, TRUE);
    }
    for (i = 0; i < nops; i++)
      if ((op = &insn->ops[i])->mode == MIR_OP_LABEL) {
        if ((label_bb_insn = op->u.label->data) == NULL) {
          create_bb (ctx, op->u.label);
          label_bb_insn = op->u.label->data;
        }
        bb_insn = insn->data;
        create_edge (ctx, bb_insn->bb, label_bb_insn->bb, TRUE);
      } else if (op->mode == MIR_OP_REG) {
        update_min_max_reg (ctx, op->u.reg);
      } else if (op->mode == MIR_OP_MEM) {
        update_min_max_reg (ctx, op->u.mem.base);
        update_min_max_reg (ctx, op->u.mem.index);
      }
  }
  /* Add additional edges with entry and exit */
  for (bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    if (bb != entry_bb && DLIST_HEAD (in_edge_t, bb->in_edges) == NULL)
      create_edge (ctx, entry_bb, bb, TRUE);
    if (bb != exit_bb && DLIST_HEAD (out_edge_t, bb->out_edges) == NULL)
      create_edge (ctx, bb, exit_bb, TRUE);
  }
  enumerate_bbs (ctx);
  VARR_CREATE (reg_info_t, curr_cfg->breg_info, 128);
}

static void destroy_func_cfg (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn;
  bb_insn_t bb_insn;
  bb_t bb, next_bb;
  mv_t mv, next_mv;

  gen_assert (curr_func_item->item_type == MIR_func_item && curr_func_item->data != NULL);
  for (insn = DLIST_HEAD (MIR_insn_t, curr_func_item->u.func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
    bb_insn = insn->data;
    gen_assert (bb_insn != NULL);
    delete_bb_insn (bb_insn);
  }
  for (bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = next_bb) {
    next_bb = DLIST_NEXT (bb_t, bb);
    delete_bb (ctx, bb);
  }
  for (mv = DLIST_HEAD (mv_t, curr_cfg->used_moves); mv != NULL; mv = next_mv) {
    next_mv = DLIST_NEXT (mv_t, mv);
    free (mv);
  }
  for (mv = DLIST_HEAD (mv_t, curr_cfg->free_moves); mv != NULL; mv = next_mv) {
    next_mv = DLIST_NEXT (mv_t, mv);
    free (mv);
  }
  VARR_DESTROY (reg_info_t, curr_cfg->breg_info);
  free (curr_func_item->data);
  curr_func_item->data = NULL;
}

static void add_new_bb_insns (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_func_t func;
  size_t i, nops;
  MIR_op_t op;
  bb_t bb = DLIST_EL (bb_t, curr_cfg->bbs, 2);
  bb_insn_t bb_insn, last_bb_insn = NULL;

  gen_assert (curr_func_item->item_type == MIR_func_item);
  func = curr_func_item->u.func;
  for (MIR_insn_t insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn))
    if (insn->data != NULL) {
      bb = (last_bb_insn = insn->data)->bb;
      if (MIR_branch_code_p (insn->code) || insn->code == MIR_RET || insn->code == MIR_SWITCH) {
        bb = DLIST_NEXT (bb_t, bb);
        last_bb_insn = NULL;
      }
    } else { /* New insn: */
      gen_assert (bb != NULL);
      bb_insn = create_bb_insn (ctx, insn, bb);
      if (last_bb_insn != NULL) {
        DLIST_INSERT_AFTER (bb_insn_t, bb->bb_insns, last_bb_insn, bb_insn);
      } else {
        gen_assert (DLIST_HEAD (bb_insn_t, bb->bb_insns) != NULL
                    && DLIST_HEAD (bb_insn_t, bb->bb_insns)->insn->code != MIR_LABEL);
        DLIST_PREPEND (bb_insn_t, bb->bb_insns, bb_insn);
      }
      last_bb_insn = bb_insn;
      nops = MIR_insn_nops (ctx, insn);
      for (i = 0; i < nops; i++) {
        op = insn->ops[i];
        if (op.mode == MIR_OP_REG) {
          update_min_max_reg (ctx, op.u.reg);
        } else if (op.mode == MIR_OP_MEM) {
          update_min_max_reg (ctx, op.u.mem.base);
          update_min_max_reg (ctx, op.u.mem.index);
        }
      }
    }
}

static int rpost_cmp (const void *a1, const void *a2) {
  return (*(const struct bb **) a1)->rpost - (*(const struct bb **) a2)->rpost;
}

static int post_cmp (const void *a1, const void *a2) { return -rpost_cmp (a1, a2); }

DEF_VARR (bb_t);

struct data_flow_ctx {
  VARR (bb_t) * worklist, *pending;
  bitmap_t bb_to_consider;
};

#define worklist gen_ctx->data_flow_ctx->worklist
#define pending gen_ctx->data_flow_ctx->pending
#define bb_to_consider gen_ctx->data_flow_ctx->bb_to_consider

static void solve_dataflow (MIR_context_t ctx, int forward_p, void (*con_func_0) (bb_t),
                            int (*con_func_n) (MIR_context_t, bb_t),
                            int (*trans_func) (MIR_context_t, bb_t)) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t i, iter;
  bb_t bb, *addr;
  VARR (bb_t) * t;

  VARR_TRUNC (bb_t, worklist, 0);
  for (bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb))
    VARR_PUSH (bb_t, worklist, bb);
  VARR_TRUNC (bb_t, pending, 0);
  iter = 0;
  while (VARR_LENGTH (bb_t, worklist) != 0) {
    VARR_TRUNC (bb_t, pending, 0);
    addr = VARR_ADDR (bb_t, worklist);
    qsort (addr, VARR_LENGTH (bb_t, worklist), sizeof (bb), forward_p ? rpost_cmp : post_cmp);
    bitmap_clear (bb_to_consider);
    for (i = 0; i < VARR_LENGTH (bb_t, worklist); i++) {
      int changed_p = iter == 0;
      edge_t e;

      bb = addr[i];
      if (forward_p) {
        if (DLIST_HEAD (in_edge_t, bb->in_edges) == NULL)
          con_func_0 (bb);
        else
          changed_p |= con_func_n (ctx, bb);
      } else {
        if (DLIST_HEAD (out_edge_t, bb->out_edges) == NULL)
          con_func_0 (bb);
        else
          changed_p |= con_func_n (ctx, bb);
      }
      if (changed_p && trans_func (ctx, bb)) {
        if (forward_p) {
          for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL;
               e = DLIST_NEXT (out_edge_t, e))
            if (bitmap_set_bit_p (bb_to_consider, e->dst->index)) VARR_PUSH (bb_t, pending, e->dst);
        } else {
          for (e = DLIST_HEAD (in_edge_t, bb->in_edges); e != NULL; e = DLIST_NEXT (in_edge_t, e))
            if (bitmap_set_bit_p (bb_to_consider, e->src->index)) VARR_PUSH (bb_t, pending, e->src);
        }
      }
    }
    iter++;
    t = worklist;
    worklist = pending;
    pending = t;
  }
}

static void init_data_flow (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->data_flow_ctx = gen_malloc (ctx, sizeof (struct data_flow_ctx));
  VARR_CREATE (bb_t, worklist, 0);
  VARR_CREATE (bb_t, pending, 0);
  bb_to_consider = bitmap_create2 (512);
}

static void finish_data_flow (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (bb_t, worklist);
  VARR_DESTROY (bb_t, pending);
  bitmap_destroy (bb_to_consider);
  free (gen_ctx->data_flow_ctx);
  gen_ctx->data_flow_ctx = NULL;
}

/* New Page */

/* Common Sub-expression Elimination.  */

#define av_in in
#define av_out out
#define av_kill kill
#define av_gen gen

typedef struct expr {
  MIR_insn_t insn;    /* operation and input operands are the expr keys */
  unsigned int num;   /* the expression number (0, 1 ...) */
  MIR_reg_t temp_reg; /* ??? */
} * expr_t;

DEF_VARR (expr_t);
DEF_HTAB (expr_t);
DEF_VARR (bitmap_t);

struct cse_ctx {
  VARR (expr_t) * exprs; /* the expr number -> expression */
  /* map: var number -> bitmap of numbers of exprs with given var as an input operand. */
  VARR (bitmap_t) * var2dep_expr;
  bitmap_t memory_exprs;    /* expressions containing memory */
  HTAB (expr_t) * expr_tab; /* keys: insn code and input operands */
  bitmap_t curr_bb_av_gen, curr_bb_av_kill;
};

#define exprs gen_ctx->cse_ctx->exprs
#define var2dep_expr gen_ctx->cse_ctx->var2dep_expr
#define memory_exprs gen_ctx->cse_ctx->memory_exprs
#define expr_tab gen_ctx->cse_ctx->expr_tab
#define curr_bb_av_gen gen_ctx->cse_ctx->curr_bb_av_gen
#define curr_bb_av_kill gen_ctx->cse_ctx->curr_bb_av_kill

static int op_eq (MIR_context_t ctx, MIR_op_t op1, MIR_op_t op2) {
  return MIR_op_eq_p (ctx, op1, op2);
}

static int expr_eq (expr_t e1, expr_t e2, void *arg) {
  MIR_context_t ctx = arg;
  size_t i, nops;
  int out_p;

  if (e1->insn->code != e2->insn->code) return FALSE;
  nops = MIR_insn_nops (ctx, e1->insn);
  for (i = 0; i < nops; i++) {
    MIR_insn_op_mode (ctx, e1->insn, i, &out_p);
    if (out_p) continue;
    if (!op_eq (ctx, e1->insn->ops[i], e2->insn->ops[i])) return FALSE;
  }
  return TRUE;
}

static htab_hash_t add_op_hash (MIR_context_t ctx, htab_hash_t h, MIR_op_t op) {
  return MIR_op_hash_step (ctx, h, op);
}

static htab_hash_t expr_hash (expr_t e, void *arg) {
  MIR_context_t ctx = arg;
  size_t i, nops;
  int out_p;
  htab_hash_t h = mir_hash_init (0x42);

  h = mir_hash_step (h, (uint64_t) e->insn->code);
  nops = MIR_insn_nops (ctx, e->insn);
  for (i = 0; i < nops; i++) {
    MIR_insn_op_mode (ctx, e->insn, i, &out_p);
    if (out_p) continue;
    h = add_op_hash (ctx, h, e->insn->ops[i]);
  }
  return mir_hash_finish (h);
}

static int find_expr (MIR_context_t ctx, MIR_insn_t insn, expr_t *e) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  struct expr es;

  es.insn = insn;
  return HTAB_DO (expr_t, expr_tab, &es, HTAB_FIND, *e);
}

static void insert_expr (MIR_context_t ctx, expr_t e) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  expr_t e2;

  gen_assert (!find_expr (ctx, e->insn, &e2));
  HTAB_DO (expr_t, expr_tab, e, HTAB_INSERT, e);
}

static void process_var (MIR_context_t ctx, MIR_reg_t var, expr_t e) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bitmap_t b;

  while (var >= VARR_LENGTH (bitmap_t, var2dep_expr)) VARR_PUSH (bitmap_t, var2dep_expr, NULL);
  if ((b = VARR_GET (bitmap_t, var2dep_expr, var)) == NULL) {
    b = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
    VARR_SET (bitmap_t, var2dep_expr, var, b);
  }
  bitmap_set_bit_p (b, e->num);
}

static void process_cse_ops (MIR_context_t ctx, MIR_op_t op, expr_t e) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  switch (op.mode) {  // ???
  case MIR_OP_REG: process_var (ctx, reg2var (gen_ctx, op.u.reg), e); break;
  case MIR_OP_HARD_REG: process_var (ctx, op.u.hard_reg, e); break;
  case MIR_OP_INT:
  case MIR_OP_UINT:
  case MIR_OP_FLOAT:
  case MIR_OP_DOUBLE:
  case MIR_OP_LDOUBLE:
  case MIR_OP_REF: break;
  case MIR_OP_MEM:
    if (op.u.mem.base != 0) process_var (ctx, reg2var (gen_ctx, op.u.mem.base), e);
    if (op.u.mem.index != 0) process_var (ctx, reg2var (gen_ctx, op.u.mem.index), e);
    bitmap_set_bit_p (memory_exprs, e->num);
    break;
  case MIR_OP_HARD_REG_MEM:
    if (op.u.hard_reg_mem.base != MIR_NON_HARD_REG) process_var (ctx, op.u.hard_reg_mem.base, e);
    if (op.u.hard_reg_mem.index != MIR_NON_HARD_REG) process_var (ctx, op.u.hard_reg_mem.index, e);
    bitmap_set_bit_p (memory_exprs, e->num);
    break;
  default: gen_assert (FALSE); /* we should not have all the rest operand here */
  }
}

static expr_t add_expr (MIR_context_t ctx, MIR_insn_t insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t i, nops;
  int out_p;
  MIR_op_mode_t mode;
  expr_t e = gen_malloc (ctx, sizeof (struct expr));

  gen_assert (!MIR_call_code_p (insn->code) && insn->code != MIR_RET);
  e->insn = insn;
  e->num = VARR_LENGTH (expr_t, exprs);
  mode = MIR_insn_op_mode (ctx, insn, 0, &out_p);
  e->temp_reg
    = gen_new_temp_reg (ctx,
                        mode == MIR_OP_FLOAT
                          ? MIR_T_F
                          : mode == MIR_OP_DOUBLE ? MIR_T_D
                                                  : mode == MIR_OP_LDOUBLE ? MIR_T_LD : MIR_T_I64,
                        curr_func_item->u.func);
  VARR_PUSH (expr_t, exprs, e);
  insert_expr (ctx, e);
  nops = MIR_insn_nops (ctx, insn);
  for (i = 0; i < nops; i++) {
    MIR_insn_op_mode (ctx, insn, i, &out_p);
    if (!out_p) process_cse_ops (ctx, insn->ops[i], e);
  }
  return e;
}

static void cse_con_func_0 (bb_t bb) { bitmap_clear (bb->av_in); }

static int cse_con_func_n (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  edge_t e, head;
  bitmap_t prev_av_in = temp_bitmap;

  bitmap_copy (prev_av_in, bb->av_in);
  head = DLIST_HEAD (in_edge_t, bb->in_edges);
  bitmap_copy (bb->av_in, head->src->av_out);
  for (e = DLIST_NEXT (in_edge_t, head); e != NULL; e = DLIST_NEXT (in_edge_t, e))
    bitmap_and (bb->av_in, bb->av_in, e->src->av_out); /* av_in &= av_out */
  return !bitmap_equal_p (bb->av_in, prev_av_in);
}

static int cse_trans_func (MIR_context_t ctx, bb_t bb) {
  return bitmap_ior_and_compl (bb->av_out, bb->av_gen, bb->av_in, bb->av_kill);
}

static void create_exprs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb))
    for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
      expr_t e;
      MIR_insn_t insn = bb_insn->insn;

      if (!MIR_branch_code_p (insn->code) && insn->code != MIR_RET && insn->code != MIR_SWITCH
          && insn->code != MIR_LABEL && !MIR_call_code_p (insn->code) && insn->code != MIR_ALLOCA
          && insn->code != MIR_BSTART && insn->code != MIR_BEND && insn->code != MIR_VA_START
          && insn->code != MIR_VA_ARG && insn->code != MIR_VA_END && !move_p (insn)
          && (!imm_move_p (insn) || insn->ops[1].mode == MIR_OP_REF)
          /* After simplification we have only one store form: mem = reg.
             It is unprofitable to add the reg as an expression.  */
          && insn->ops[0].mode != MIR_OP_MEM && insn->ops[0].mode != MIR_OP_HARD_REG_MEM
          && !find_expr (ctx, insn, &e))
        add_expr (ctx, insn);
    }
}

static void make_obsolete_var_exprs (MIR_context_t ctx, size_t nel) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t var = nel;
  bitmap_t b;

  if (var < VARR_LENGTH (bitmap_t, var2dep_expr)
      && (b = VARR_GET (bitmap_t, var2dep_expr, var)) != NULL) {
    if (curr_bb_av_gen != NULL) bitmap_and_compl (curr_bb_av_gen, curr_bb_av_gen, b);
    if (curr_bb_av_kill != NULL) bitmap_ior (curr_bb_av_kill, curr_bb_av_kill, b);
  }
}

static void create_av_bitmaps (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t nel, exprs_num = VARR_LENGTH (expr_t, exprs);
  bitmap_t all_expr_bitmap = temp_bitmap2;
  bitmap_iterator_t bi;

  bitmap_clear (all_expr_bitmap);
  bitmap_set_bit_range_p (all_expr_bitmap, 0, exprs_num);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    bitmap_clear (bb->av_in);
    bitmap_clear (bb->av_out);
    if (bb != DLIST_HEAD (bb_t, curr_cfg->bbs)) bitmap_copy (bb->av_out, all_expr_bitmap);
    bitmap_clear (bb->av_kill);
    bitmap_clear (bb->av_gen);
    curr_bb_av_gen = bb->av_gen;
    curr_bb_av_kill = bb->av_kill;
    for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
      size_t i, nops;
      int out_p;
      MIR_reg_t early_clobbered_hard_reg1, early_clobbered_hard_reg2;
      MIR_op_t op;
      expr_t e;
      MIR_insn_t insn = bb_insn->insn;

      if (MIR_branch_code_p (bb_insn->insn->code) || insn->code == MIR_RET
          || insn->code == MIR_SWITCH || insn->code == MIR_LABEL)
        continue;
      if (!MIR_call_code_p (insn->code) && insn->code != MIR_ALLOCA && insn->code != MIR_BSTART
          && insn->code != MIR_BEND && insn->code != MIR_VA_START && insn->code != MIR_VA_ARG
          && insn->code != MIR_VA_END && !move_p (insn)
          && (!imm_move_p (insn) || insn->ops[1].mode == MIR_OP_REF)
          /* See create_expr comments: */
          && insn->ops[0].mode != MIR_OP_MEM && insn->ops[0].mode != MIR_OP_HARD_REG_MEM) {
        if (!find_expr (ctx, insn, &e)) {
          gen_assert (FALSE);
          continue;
        }
        bitmap_set_bit_p (bb->av_gen, e->num);
      }
      target_get_early_clobbered_hard_regs (insn, &early_clobbered_hard_reg1,
                                            &early_clobbered_hard_reg2);
      if (early_clobbered_hard_reg1 != MIR_NON_HARD_REG)
        make_obsolete_var_exprs (ctx, early_clobbered_hard_reg1);
      if (early_clobbered_hard_reg2 != MIR_NON_HARD_REG)
        make_obsolete_var_exprs (ctx, early_clobbered_hard_reg2);
      nops = MIR_insn_nops (ctx, insn);
      for (i = 0; i < nops; i++) {
        MIR_insn_op_mode (ctx, insn, i, &out_p);
        op = insn->ops[i];
        if (!out_p) continue;
        if (op.mode == MIR_OP_MEM || op.mode == MIR_OP_HARD_REG_MEM) {
          bitmap_and_compl (bb->av_gen, bb->av_gen, memory_exprs);
          bitmap_ior (bb->av_kill, bb->av_kill, memory_exprs);
        } else if (op.mode == MIR_OP_REG || op.mode == MIR_OP_HARD_REG) {
          make_obsolete_var_exprs (ctx, op.mode == MIR_OP_HARD_REG ? op.u.hard_reg
                                                                   : reg2var (gen_ctx, op.u.reg));
        }
      }
      if (MIR_call_code_p (insn->code)) {
        gen_assert (bb_insn->call_hard_reg_args != NULL);
        FOREACH_BITMAP_BIT (bi, bb_insn->call_hard_reg_args, nel) {
          make_obsolete_var_exprs (ctx, nel);
        }
        FOREACH_BITMAP_BIT (bi, call_used_hard_regs, nel) { make_obsolete_var_exprs (ctx, nel); }
        bitmap_and_compl (bb->av_gen, bb->av_gen, memory_exprs);
        bitmap_ior (bb->av_kill, bb->av_kill, memory_exprs);
      }
    }
  }
}

static void cse_modify (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_insn_t bb_insn, new_bb_insn, next_bb_insn;
  bitmap_t av = temp_bitmap;
  size_t nel;
  bitmap_iterator_t bi;

  curr_bb_av_gen = temp_bitmap;
  curr_bb_av_kill = NULL;
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    bitmap_copy (av, bb->av_in);
    for (bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL; bb_insn = next_bb_insn) {
      size_t i, nops;
      expr_t e;
      MIR_reg_t early_clobbered_hard_reg1, early_clobbered_hard_reg2;
      MIR_op_t op;
      int out_p;
      MIR_type_t type;
      MIR_insn_code_t move_code;
      MIR_insn_t new_insn, insn = bb_insn->insn;

      next_bb_insn = DLIST_NEXT (bb_insn_t, bb_insn);
      if (MIR_branch_code_p (insn->code) || insn->code == MIR_RET || insn->code == MIR_SWITCH
          || insn->code == MIR_LABEL)
        continue;
      if (!MIR_call_code_p (insn->code) && insn->code != MIR_ALLOCA && insn->code != MIR_BSTART
          && insn->code != MIR_BEND && insn->code != MIR_VA_START && insn->code != MIR_VA_ARG
          && insn->code != MIR_VA_END && !move_p (insn)
          && (!imm_move_p (insn) || insn->ops[1].mode == MIR_OP_REF)
          /* See create_expr comments: */
          && insn->ops[0].mode != MIR_OP_MEM && insn->ops[0].mode != MIR_OP_HARD_REG_MEM) {
        if (!find_expr (ctx, insn, &e)) {
          gen_assert (FALSE);
          continue;
        }
        op = MIR_new_reg_op (ctx, e->temp_reg);
        type = MIR_reg_type (ctx, e->temp_reg, curr_func_item->u.func);
        move_code
          = (type == MIR_T_F ? MIR_FMOV
                             : type == MIR_T_D ? MIR_DMOV : type == MIR_T_LD ? MIR_LDMOV : MIR_MOV);
#ifndef NDEBUG
        MIR_insn_op_mode (ctx, insn, 0, &out_p); /* result here is always 0-th op */
        gen_assert (out_p);
#endif
        if (!bitmap_bit_p (av, e->num)) {
          bitmap_set_bit_p (av, e->num);
          new_insn = MIR_new_insn (ctx, move_code, op, insn->ops[0]);
          new_bb_insn = create_bb_insn (ctx, new_insn, bb);
          MIR_insert_insn_after (ctx, curr_func_item, insn, new_insn);
          DLIST_INSERT_AFTER (bb_insn_t, bb->bb_insns, bb_insn, new_bb_insn);
          next_bb_insn = DLIST_NEXT (bb_insn_t, new_bb_insn);
#if !MIR_NO_GEN_DEBUG
          if (debug_file != NULL) {
            fprintf (debug_file, "  adding insn ");
            MIR_output_insn (ctx, debug_file, new_insn, curr_func_item->u.func, FALSE);
            fprintf (debug_file, "  after def insn ");
            MIR_output_insn (ctx, debug_file, insn, curr_func_item->u.func, TRUE);
          }
#endif
        } else {
          new_insn = MIR_new_insn (ctx, move_code, insn->ops[0], op);
          gen_add_insn_after (ctx, insn, new_insn);
#if !MIR_NO_GEN_DEBUG
          if (debug_file != NULL) {
            fprintf (debug_file, "  adding insn ");
            MIR_output_insn (ctx, debug_file, new_insn, curr_func_item->u.func, FALSE);
            fprintf (debug_file, "  after use insn ");
            MIR_output_insn (ctx, debug_file, insn, curr_func_item->u.func, TRUE);
          }
#endif
          insn = new_insn;
        }
      }
      target_get_early_clobbered_hard_regs (insn, &early_clobbered_hard_reg1,
                                            &early_clobbered_hard_reg2);
      if (early_clobbered_hard_reg1 != MIR_NON_HARD_REG)
        make_obsolete_var_exprs (ctx, early_clobbered_hard_reg1);
      if (early_clobbered_hard_reg2 != MIR_NON_HARD_REG)
        make_obsolete_var_exprs (ctx, early_clobbered_hard_reg2);
      nops = MIR_insn_nops (ctx, insn);
      for (i = 0; i < nops; i++) {
        op = insn->ops[i];
        MIR_insn_op_mode (ctx, insn, i, &out_p);
        if (!out_p) continue;
        if (op.mode == MIR_OP_MEM || op.mode == MIR_OP_HARD_REG_MEM) {
          bitmap_and_compl (av, av, memory_exprs);
        } else if (op.mode == MIR_OP_REG || op.mode == MIR_OP_HARD_REG) {
          make_obsolete_var_exprs (ctx, op.mode == MIR_OP_HARD_REG ? op.u.hard_reg
                                                                   : reg2var (gen_ctx, op.u.reg));
        }
      }
      if (MIR_call_code_p (insn->code)) {
        gen_assert (bb_insn->call_hard_reg_args != NULL);
        FOREACH_BITMAP_BIT (bi, bb_insn->call_hard_reg_args, nel) {
          make_obsolete_var_exprs (ctx, nel);
        }
        FOREACH_BITMAP_BIT (bi, call_used_hard_regs, nel) { make_obsolete_var_exprs (ctx, nel); }
        bitmap_and_compl (av, av, memory_exprs);
      }
    }
  }
}

static void cse (MIR_context_t ctx) {
  create_exprs (ctx);
  create_av_bitmaps (ctx);
  solve_dataflow (ctx, TRUE, cse_con_func_0, cse_con_func_n, cse_trans_func);
  cse_modify (ctx);
}

#if !MIR_NO_GEN_DEBUG
static void print_exprs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  fprintf (debug_file, "  Expressions:\n");
  for (size_t i = 0; i < VARR_LENGTH (expr_t, exprs); i++) {
    size_t nops;
    expr_t e = VARR_GET (expr_t, exprs, i);

    fprintf (debug_file, "  %3lu: ", (unsigned long) i);
    fprintf (debug_file, "%s _", MIR_insn_name (ctx, e->insn->code));
    nops = MIR_insn_nops (ctx, e->insn);
    for (size_t j = 1; j < nops; j++) {
      fprintf (debug_file, ", ");
      MIR_output_op (ctx, debug_file, e->insn->ops[j], curr_func_item->u.func);
    }
    fprintf (debug_file, "\n");
  }
}

static void output_bb_cse_info (MIR_context_t ctx, bb_t bb) {
  output_bitmap (ctx, "  av_in:", bb->av_in, TRUE);
  output_bitmap (ctx, "  av_out:", bb->av_out, TRUE);
  output_bitmap (ctx, "  av_gen:", bb->av_gen, TRUE);
  output_bitmap (ctx, "  av_kill:", bb->av_kill, TRUE);
}
#endif

static void cse_clear (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  HTAB_CLEAR (expr_t, expr_tab);
  while (VARR_LENGTH (expr_t, exprs) != 0) free (VARR_POP (expr_t, exprs));
  while (VARR_LENGTH (bitmap_t, var2dep_expr) != 0) {
    bitmap_t b = VARR_POP (bitmap_t, var2dep_expr);

    if (b != NULL) bitmap_destroy (b);
  }
  bitmap_clear (memory_exprs);
}

static void init_cse (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->cse_ctx = gen_malloc (ctx, sizeof (struct cse_ctx));
  VARR_CREATE (expr_t, exprs, 512);
  VARR_CREATE (bitmap_t, var2dep_expr, 512);
  memory_exprs = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  HTAB_CREATE (expr_t, expr_tab, 1024, expr_hash, expr_eq, ctx);
}

static void finish_cse (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (expr_t, exprs);
  bitmap_destroy (memory_exprs);
  VARR_DESTROY (bitmap_t, var2dep_expr);
  HTAB_DESTROY (expr_t, expr_tab);
  free (gen_ctx->cse_ctx);
  gen_ctx->cse_ctx = NULL;
}

#undef av_in
#undef av_out
#undef av_kill
#undef av_gen

/* New Page */

/* Reaching definitions. */

#define rdef_in in
#define rdef_out out
#define rdef_kill kill
#define rdef_gen gen

DEF_VARR (bb_insn_t);

struct rdef_ctx {
  VARR (bb_insn_t) * def_bb_insns;
  VARR (bitmap_t) * var_uses, *var_defs;
  bitmap_t artificial_def_bitmap, curr_rdef_bitmap;
};

#define def_bb_insns gen_ctx->rdef_ctx->def_bb_insns
#define var_uses gen_ctx->rdef_ctx->var_uses
#define var_defs gen_ctx->rdef_ctx->var_defs
#define artificial_def_bitmap gen_ctx->rdef_ctx->artificial_def_bitmap
#define curr_rdef_bitmap gen_ctx->rdef_ctx->curr_rdef_bitmap

static void rdef_con_func_0 (bb_t bb) { bitmap_clear (bb->rdef_in); }

static int rdef_con_func_n (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  int first_bb_p = bb == DLIST_EL (bb_t, curr_cfg->bbs, 2);
  edge_t e, head;
  bitmap_t prev_rdef_in = temp_bitmap;

  if (first_bb_p) bitmap_ior (bb->rdef_in, bb->rdef_in, artificial_def_bitmap);
  bitmap_copy (prev_rdef_in, bb->rdef_in);
  head = DLIST_HEAD (in_edge_t, bb->in_edges);
  bitmap_copy (bb->rdef_in, head->src->rdef_out);
  for (e = DLIST_NEXT (in_edge_t, head); e != NULL; e = DLIST_NEXT (in_edge_t, e))
    bitmap_ior (bb->rdef_in, bb->rdef_in, e->src->rdef_out); /* rdef_in |= rdef_out */
  if (first_bb_p) bitmap_ior (bb->rdef_in, bb->rdef_in, artificial_def_bitmap);
  return !bitmap_equal_p (bb->rdef_in, prev_rdef_in);
}

static int rdef_trans_func (MIR_context_t ctx, bb_t bb) {
  return bitmap_ior_and_compl (bb->rdef_out, bb->rdef_gen, bb->rdef_in, bb->rdef_kill);
}

static bitmap_t get_var_uses_or_defs (MIR_context_t ctx, MIR_reg_t var, int uses_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bitmap_t bm;
  VARR (bitmap_t) *bitmap_varr = uses_p ? var_uses : var_defs;

  while (VARR_LENGTH (bitmap_t, bitmap_varr) <= var) VARR_PUSH (bitmap_t, bitmap_varr, NULL);
  if ((bm = VARR_GET (bitmap_t, bitmap_varr, var)) != NULL) return bm;
  bm = bitmap_create2 (2 * curr_cfg->max_reg);
  VARR_SET (bitmap_t, bitmap_varr, var, bm);
  return bm;
}

static void calculate_reaching_defs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_func_t func = curr_func_item->u.func;
  bb_t entry_bb = DLIST_HEAD (bb_t, curr_cfg->bbs);
  int out_p, mem_p;
  size_t i, passed_mem_num, n_out, def;
  MIR_reg_t var;
  const char *var_name;
  bitmap_t def_bitmap;
  insn_var_iterator_t iter;

  VARR_TRUNC (bb_insn_t, def_bb_insns, 0);
  for (bb_t bb = entry_bb; bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    /* Set up: var -> def insn indexes; bb_insn -> insn index; var = use insn indexes */
    for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
      n_out = 0;
      bb_insn->index = VARR_LENGTH (bb_insn_t, def_bb_insns);
      VARR_PUSH (bb_insn_t, def_bb_insns, bb_insn);
      FOREACH_INSN_VAR (ctx, iter, bb_insn->insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p) {
          bitmap_set_bit_p (get_var_uses_or_defs (ctx, var, TRUE), bb_insn->index);
        } else {
          if (n_out != 0) VARR_PUSH (bb_insn_t, def_bb_insns, bb_insn);
          bitmap_set_bit_p (get_var_uses_or_defs (ctx, var, FALSE), bb_insn->index + n_out++);
        }
      }
    }
  }
  /* Add special defs for each fucn arg */
  bitmap_clear (artificial_def_bitmap);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL && func->nargs > 0)
    fprintf (debug_file, " Adding special defs name(reg,def):");
#endif
  for (i = 0; i < func->nargs; i++) {
    var_name = VARR_GET (MIR_var_t, func->vars, i).name;
    var = reg2var (gen_ctx, MIR_reg (ctx, var_name, func));
    gen_assert (var_is_reg_p (var));
    def_bitmap = get_var_uses_or_defs (ctx, var, FALSE);
    def = VARR_LENGTH (bb_insn_t, def_bb_insns);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL)
      fprintf (debug_file, " %s(%lu,%lu)", var_name, (unsigned long) var2reg (gen_ctx, var),
               (unsigned long) def);
#endif
    VARR_PUSH (bb_insn_t, def_bb_insns, NULL);
    bitmap_set_bit_p (def_bitmap, def);
    bitmap_set_bit_p (artificial_def_bitmap, def);
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL && i > 0) fprintf (debug_file, "\n");
#endif
  for (bb_t bb = entry_bb; bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    /* Set up rdef gen/kill; initialize rdef in/out */
    bitmap_clear (bb->rdef_in);
    bitmap_clear (bb->rdef_out);
    bitmap_clear (bb->rdef_gen);
    bitmap_clear (bb->rdef_kill);
    for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
      n_out = 0;
      FOREACH_INSN_VAR (ctx, iter, bb_insn->insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p) continue;
        def_bitmap = get_var_uses_or_defs (ctx, var, FALSE);
        gen_assert (def_bitmap != NULL);
        bitmap_ior (bb->rdef_kill, bb->rdef_kill, def_bitmap);
        bitmap_and_compl (bb->rdef_gen, bb->rdef_gen, def_bitmap);
        bitmap_set_bit_p (bb->rdef_gen, bb_insn->index + n_out++);
      }
    }
  }
  solve_dataflow (ctx, TRUE, rdef_con_func_0, rdef_con_func_n, rdef_trans_func);
}

#if !MIR_NO_GEN_DEBUG
static void output_bb_rdef_info (MIR_context_t ctx, bb_t bb) {
  output_bitmap (ctx, "  rdef_in:", bb->rdef_in, FALSE);
  output_bitmap (ctx, "  rdef_out:", bb->rdef_out, FALSE);
  output_bitmap (ctx, "  rdef_gen:", bb->rdef_gen, FALSE);
  output_bitmap (ctx, "  rdef_kill:", bb->rdef_kill, FALSE);
}
#endif

static void rdef_clear (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bitmap_t bm;

  while (VARR_LENGTH (bitmap_t, var_uses) != 0)
    if ((bm = VARR_POP (bitmap_t, var_uses)) != NULL) bitmap_destroy (bm);
  while (VARR_LENGTH (bitmap_t, var_defs) != 0)
    if ((bm = VARR_POP (bitmap_t, var_defs)) != NULL) bitmap_destroy (bm);
}

static void init_rdef (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->rdef_ctx = gen_malloc (ctx, sizeof (struct rdef_ctx));
  VARR_CREATE (bb_insn_t, def_bb_insns, 0);
  VARR_CREATE (bitmap_t, var_uses, 0);
  VARR_CREATE (bitmap_t, var_defs, 0);
  artificial_def_bitmap = bitmap_create ();
  curr_rdef_bitmap = bitmap_create ();
}

static void finish_rdef (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (bb_insn_t, def_bb_insns);
  VARR_DESTROY (bitmap_t, var_uses);
  VARR_DESTROY (bitmap_t, var_defs);
  bitmap_destroy (artificial_def_bitmap);
  bitmap_destroy (curr_rdef_bitmap);
  free (gen_ctx->rdef_ctx);
  gen_ctx->rdef_ctx = NULL;
}

#undef rdef_in
#undef rdef_out
#undef rdef_kill
#undef rdef_gen

/* New Page */

/* Register (variable) renaming.  Reaching definitions info should exist. */

#define rdef_in in
#define rdef_out out
#define rdef_kill kill
#define rdef_gen gen

struct web_node {
  MIR_reg_t var;      /* web_node_tab key */
  int def_p;          /* web_node_tab key: TRUE for definition, FALSE otherwise */
  bb_insn_t bb_insn;  /* web_node_tab key: NULL for an artificial definition */
  size_t n_out;       /* web_node_tab key: output number of insn, 0 for input */
  size_t first, next; /* first, next node index in the web: 0 is NULL */
};

typedef struct web_node web_node_t;

DEF_VARR (web_node_t);
DEF_HTAB (size_t);
DEF_VARR (size_t);
DEF_VARR (char);

struct rename_ctx {
  VARR (web_node_t) * web_nodes;
  HTAB (size_t) * web_node_htab;
  VARR (size_t) * curr_var_indexes;
  VARR (char) * reg_name;
};

#define web_nodes gen_ctx->rename_ctx->web_nodes
#define web_node_htab gen_ctx->rename_ctx->web_node_htab
#define curr_var_indexes gen_ctx->rename_ctx->curr_var_indexes
#define reg_name gen_ctx->rename_ctx->reg_name

static int web_node_eq (size_t wn_ind1, size_t wn_ind2, void *arg) {
  MIR_context_t ctx = arg;
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  web_node_t *addr = VARR_ADDR (web_node_t, web_nodes), *n1 = &addr[wn_ind1], *n2 = &addr[wn_ind2];

  return (n1->bb_insn == n2->bb_insn && n1->n_out == n2->n_out && n1->var == n2->var
          && n1->def_p == n2->def_p);
  return 0;
}

static htab_hash_t web_node_hash (size_t wn_ind, void *arg) {
  MIR_context_t ctx = arg;
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  web_node_t *addr = VARR_ADDR (web_node_t, web_nodes);
  htab_hash_t h = mir_hash_init (0x24);

  if (addr[wn_ind].bb_insn != NULL) h = mir_hash_step (h, (uint64_t) addr[wn_ind].bb_insn);
  h = mir_hash_step (h, (uint64_t) addr[wn_ind].n_out);
  h = mir_hash_step (h, (uint64_t) addr[wn_ind].var);
  h = mir_hash_step (h, (uint64_t) addr[wn_ind].def_p);
  return mir_hash_finish (h);
  return 0;
}

static size_t get_web_node_ind (MIR_context_t ctx, MIR_reg_t var, int def_p, bb_insn_t bb_insn,
                                size_t n_out) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t tab_el, ind = VARR_LENGTH (web_node_t, web_nodes);
  web_node_t wn = (struct web_node){var, def_p, bb_insn, n_out, ind, 0};

  VARR_PUSH (web_node_t, web_nodes, wn);
  if (!HTAB_DO (size_t, web_node_htab, ind, HTAB_INSERT, tab_el)) {
    gen_assert (ind == tab_el);
    return ind;
  }
  VARR_POP (web_node_t, web_nodes);
  return tab_el;
}

static void link_web_nodes (MIR_context_t ctx, size_t ind1, size_t ind2) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  web_node_t *addr = VARR_ADDR (web_node_t, web_nodes);
  size_t curr, last, first1 = addr[ind1].first, first2 = addr[ind2].first;

  if (first1 == first2) return; /* already linked */
  for (curr = ind1; curr != 0; curr = addr[curr].next) last = curr;
  for (curr = first2; curr != 0; curr = addr[curr].next) addr[curr].first = first1;
  addr[last].next = first2;
}

static void build_webs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t var;
  bb_insn_t bb_insn, def_bb_insn;
  int out_p, mem_p;
  size_t nbit, passed_mem_num, web_node_ind, web_node_ind2, n_out;
  web_node_t wn;
  bitmap_t def_bitmap = temp_bitmap, var_rdef_bitmap = temp_bitmap2;
  bitmap_iterator_t bi;
  insn_var_iterator_t iter;

  HTAB_CLEAR (size_t, web_node_htab);
  VARR_TRUNC (web_node_t, web_nodes, 0);
  VARR_PUSH (web_node_t, web_nodes, wn); /* to avoid nodes with 0-th index */
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    bitmap_copy (curr_rdef_bitmap, bb->rdef_in);
    for (bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
      FOREACH_INSN_VAR (ctx, iter, bb_insn->insn, var, out_p, mem_p, passed_mem_num) {
        if (!var_is_reg_p (var) || out_p) continue; /* ignore hard regs here */
        bitmap_and (var_rdef_bitmap, curr_rdef_bitmap, get_var_uses_or_defs (ctx, var, FALSE));
        web_node_ind = get_web_node_ind (ctx, var, FALSE, bb_insn, 0);
        if (bitmap_empty_p (var_rdef_bitmap)) {
          web_node_ind2 = get_web_node_ind (ctx, var, TRUE, NULL, 0); /* an artificial def */
          link_web_nodes (ctx, web_node_ind, web_node_ind2);
        } else {
          FOREACH_BITMAP_BIT (bi, var_rdef_bitmap, nbit) {
            def_bb_insn = VARR_GET (bb_insn_t, def_bb_insns, nbit);
            gen_assert (def_bb_insn == NULL || nbit >= def_bb_insn->index);
            web_node_ind2 = get_web_node_ind (ctx, var, TRUE, def_bb_insn,
                                              def_bb_insn == NULL ? 0 : nbit - def_bb_insn->index);
            link_web_nodes (ctx, web_node_ind, web_node_ind2);
          }
        }
      }
      /* Update curr_rdef_bitmap: */
      n_out = 0;
      FOREACH_INSN_VAR (ctx, iter, bb_insn->insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p) continue;
        def_bitmap = get_var_uses_or_defs (ctx, var, FALSE);
        gen_assert (def_bitmap != NULL);
        bitmap_and_compl (curr_rdef_bitmap, curr_rdef_bitmap, def_bitmap);
        bitmap_set_bit_p (curr_rdef_bitmap, bb_insn->index + n_out++);
      }
    }
  }
}

static MIR_reg_t get_new_reg (MIR_context_t ctx, MIR_reg_t reg, size_t index) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_func_t func = curr_func_item->u.func;
  MIR_type_t type = MIR_reg_type (ctx, reg, func);
  const char *name = MIR_reg_name (ctx, reg, func);
  char ind_str[20];
  MIR_reg_t new_reg;

  VARR_TRUNC (char, reg_name, 0);
  VARR_PUSH_ARR (char, reg_name, name, strlen (name));
  VARR_PUSH (char, reg_name, '@');
  sprintf (ind_str, "%lu", (unsigned long) index); /* ??? should be enough to unique */
  VARR_PUSH_ARR (char, reg_name, ind_str, strlen (ind_str) + 1);
  new_reg = MIR_new_func_reg (ctx, func, type, VARR_ADDR (char, reg_name));
  update_min_max_reg (ctx, new_reg);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL)
    fprintf (debug_file, " Renaming %s to %s in", name, VARR_ADDR (char, reg_name));
#endif
  return new_reg;
}

static void reg_rename (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_func_t func = curr_func_item->u.func;
  MIR_insn_t insn;
  MIR_reg_t var, reg, new_reg;
  MIR_op_t op, *ops;
  bb_insn_t bb_insn;
  size_t i, j, nops, index, curr;
  int def_p, out_p, change_p;
  web_node_t *addr;

  build_webs (ctx);
  VARR_TRUNC (size_t, curr_var_indexes, 0);
  addr = VARR_ADDR (web_node_t, web_nodes);
  for (i = 1; i < VARR_LENGTH (web_node_t, web_nodes); i++) {
    if (addr[i].first != i) continue;
    var = addr[i].var;
    reg = var2reg (gen_ctx, var);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "web reg=%d(%s):", reg, MIR_reg_name (ctx, reg, func));
      for (curr = i; curr != 0; curr = addr[curr].next)
        if (addr[curr].bb_insn == NULL)
          fprintf (debug_file, " art_def(%s)", addr[curr].def_p ? "def" : "use");
        else
          fprintf (debug_file, " %lu(%s)",
                   (long unsigned) addr[curr].bb_insn->index + addr[curr].n_out,
                   addr[curr].def_p ? "def" : "use");
      fprintf (debug_file, "\n");
    }
#endif
    while (VARR_LENGTH (size_t, curr_var_indexes) <= var) VARR_PUSH (size_t, curr_var_indexes, 0);
    index = VARR_GET (size_t, curr_var_indexes, var);
    VARR_SET (size_t, curr_var_indexes, var, index + 1);
    if (index == 0) continue; /* use the old names */
    new_reg = get_new_reg (ctx, reg, index);
    for (curr = i; curr != 0; curr = addr[curr].next) {
      bb_insn = addr[curr].bb_insn;
      if (bb_insn == NULL) continue; /* an artificial def */
      insn = bb_insn->insn;
      def_p = addr[curr].def_p;
      nops = MIR_insn_nops (ctx, insn);
      ops = insn->ops;
      for (j = 0; j < nops; j++) {
        MIR_insn_op_mode (ctx, insn, j, &out_p);
        op = ops[j];
        if (op.mode == MIR_OP_MEM) out_p = FALSE;
        if ((def_p && !out_p) || (!def_p && out_p)) continue;
        change_p = FALSE;
        if (op.mode == MIR_OP_REG && op.u.reg == reg) {
          ops[j].u.reg = new_reg;
          change_p = TRUE;
        } else if (op.mode == MIR_OP_MEM) {
          if (op.u.mem.base == reg) {
            ops[j].u.mem.base = new_reg;
            change_p = TRUE;
          }
          if (op.u.mem.index == reg) {
            ops[j].u.mem.index = new_reg;
            change_p = TRUE;
          }
        }
#if !MIR_NO_GEN_DEBUG
        if (change_p && debug_file != NULL)
          fprintf (debug_file, " %lu(%s)", (unsigned long) bb_insn->index + addr[curr].n_out,
                   def_p ? "def" : "use");
#endif
      }
    }
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) fprintf (debug_file, "\n");
#endif
  }
}

static void init_rename (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->rename_ctx = gen_malloc (ctx, sizeof (struct rename_ctx));
  VARR_CREATE (web_node_t, web_nodes, 4096);
  HTAB_CREATE (size_t, web_node_htab, 4096, web_node_hash, web_node_eq, ctx);
  VARR_CREATE (size_t, curr_var_indexes, 4096);
  VARR_CREATE (char, reg_name, 20);
}

static void finish_rename (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (web_node_t, web_nodes);
  HTAB_DESTROY (size_t, web_node_htab);
  VARR_DESTROY (size_t, curr_var_indexes);
  VARR_DESTROY (char, reg_name);
  free (gen_ctx->rename_ctx);
  gen_ctx->rename_ctx = NULL;
}

#undef rdef_in
#undef rdef_out
#undef rdef_kill
#undef rdef_gen

/* New Page */

/* Loop Invariant Code Motion.  Reaching defs should be present for it. */

#define rdef_in in
#define rdef_out out
#define rdef_kill kill
#define rdef_gen gen

struct licm_ctx {
  VARR (bb_t) * licm_temp_bbs, *loop_bbs, *curr_exit_loop_bbs;
  VARR (bb_insn_t) * loop_stores, *invariant_bb_insns;
  bitmap_t loop_defs_bitmap;
};

#define licm_temp_bbs gen_ctx->licm_ctx->licm_temp_bbs
#define loop_bbs gen_ctx->licm_ctx->loop_bbs
#define loop_stores gen_ctx->licm_ctx->loop_stores
#define curr_exit_loop_bbs gen_ctx->licm_ctx->curr_exit_loop_bbs
#define invariant_bb_insns gen_ctx->licm_ctx->invariant_bb_insns
#define loop_defs_bitmap gen_ctx->licm_ctx->loop_defs_bitmap

static edge_t find_loop_entry_edge (MIR_context_t ctx, bb_t loop_entry) {
  edge_t e, entry_e = NULL;
  bb_insn_t head, tail;

  for (e = DLIST_HEAD (in_edge_t, loop_entry->in_edges); e != NULL; e = DLIST_NEXT (in_edge_t, e)) {
    if (e->back_edge_p) continue;
    if (entry_e != NULL) return NULL;
    entry_e = e;
  }
  gen_assert (entry_e != NULL);
  tail = DLIST_TAIL (bb_insn_t, entry_e->src->bb_insns);
  head = DLIST_HEAD (bb_insn_t, entry_e->dst->bb_insns);
  if (tail != NULL && head != NULL && DLIST_NEXT (MIR_insn_t, tail->insn) != head->insn)
    return NULL; /* not fall through */
  return entry_e;
}

static void create_preheader_from_edge (MIR_context_t ctx, edge_t e, loop_node_t loop) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_t new_bb = create_bb (ctx, NULL);
  loop_node_t bb_loop_node = create_loop_node (ctx, new_bb), parent = loop->parent;

  add_bb (ctx, new_bb);
  DLIST_REMOVE (bb_t, curr_cfg->bbs, new_bb);
  DLIST_INSERT_BEFORE (bb_t, curr_cfg->bbs, e->dst, new_bb); /* insert before loop entry */
  create_edge (ctx, e->src, new_bb, FALSE); /* fall through should be the 1st edge */
  create_edge (ctx, new_bb, e->dst, FALSE);
  delete_edge (e);
  gen_assert (parent != NULL);
  DLIST_APPEND (loop_node_t, parent->children, bb_loop_node);
  bb_loop_node->parent = parent;
  loop->preheader = new_bb;
}

static void licm_add_loop_preheaders (MIR_context_t ctx, loop_node_t loop) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_insn_t bb_insn;
  edge_t e;

  for (loop_node_t node = DLIST_HEAD (loop_node_t, loop->children); node != NULL;
       node = DLIST_NEXT (loop_node_t, node))
    if (node->bb == NULL) /* process sub-loops */
      licm_add_loop_preheaders (ctx, node);
  if (loop == curr_cfg->root_loop_node) return;
  loop->preheader = NULL;
  if ((e = find_loop_entry_edge (ctx, loop->entry->bb)) == NULL) return;
  if ((bb_insn = DLIST_TAIL (bb_insn_t, e->src->bb_insns)) == NULL
      || !MIR_branch_code_p (bb_insn->insn->code))
    loop->preheader = e->src; /* The preheader already exists */
  else
    create_preheader_from_edge (ctx, e, loop);
}

static void dom_con_func_0 (bb_t bb) { bitmap_clear (bb->dom_in); }

static int dom_con_func_n (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  edge_t e, head;
  bitmap_t prev_dom_in = temp_bitmap;

  bitmap_copy (prev_dom_in, bb->dom_in);
  head = DLIST_HEAD (in_edge_t, bb->in_edges);
  bitmap_copy (bb->dom_in, head->src->dom_out);
  for (e = DLIST_NEXT (in_edge_t, head); e != NULL; e = DLIST_NEXT (in_edge_t, e))
    bitmap_and (bb->dom_in, bb->dom_in, e->src->dom_out); /* dom_in &= dom_out */
  return !bitmap_equal_p (bb->dom_in, prev_dom_in);
}

static int dom_trans_func (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  bitmap_clear (temp_bitmap);
  bitmap_set_bit_p (temp_bitmap, bb->index);
  return bitmap_ior (bb->dom_out, bb->dom_in, temp_bitmap);
}

static void calculate_dominators (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_t entry_bb = DLIST_HEAD (bb_t, curr_cfg->bbs);

  bitmap_clear (entry_bb->dom_out);
  for (bb_t bb = DLIST_NEXT (bb_t, entry_bb); bb != NULL; bb = DLIST_NEXT (bb_t, bb))
    bitmap_set_bit_range_p (bb->dom_out, 0, curr_bb_index);
  solve_dataflow (ctx, TRUE, dom_con_func_0, dom_con_func_n, dom_trans_func);
}

static int invariant_reaching_definitions_p (MIR_context_t ctx, loop_node_t loop, MIR_reg_t var,
                                             bitmap_t var_rdefs) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t nbit;
  bb_insn_t bb_insn;
  bitmap_iterator_t iter;

  FOREACH_BITMAP_BIT (iter, var_rdefs, nbit) {
    bb_insn = VARR_GET (bb_insn_t, def_bb_insns, nbit);
    if (bb_insn == NULL) continue; /* a special def insn */
    if (in_loop_p (bb_insn->bb->loop_node, loop) && !bb_insn->flag) return FALSE;
  }
  return TRUE;
}

static int dominate_curr_loop_exits_p (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  for (size_t i = 0; i < VARR_LENGTH (bb_t, curr_exit_loop_bbs); i++)
    if (!bitmap_bit_p (VARR_GET (bb_t, curr_exit_loop_bbs, i)->dom_out, bb->index)) return FALSE;
  return TRUE;
}

static int dominate_uses_p (MIR_context_t ctx, bb_insn_t def_bb_insn, MIR_reg_t var) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bitmap_iterator_t iter;
  bb_insn_t use_bb_insn;
  bb_t use_bb, def_bb = def_bb_insn->bb;
  bitmap_t var_defs_bitmap = get_var_uses_or_defs (ctx, var, FALSE);
  size_t nbit;

  bitmap_and (temp_bitmap, get_var_uses_or_defs (ctx, var, TRUE), loop_defs_bitmap);
  FOREACH_BITMAP_BIT (iter, temp_bitmap, nbit) {
    use_bb_insn = VARR_GET (bb_insn_t, def_bb_insns, nbit);
    use_bb = use_bb_insn->bb;
    if (use_bb == def_bb) {
      if (use_bb_insn->index <= def_bb_insn->index) return FALSE;
      continue; /* def before use in the same BB */
    }
    bitmap_and (temp_bitmap2, use_bb->rdef_in, var_defs_bitmap);
    if (!bitmap_clear_bit_p (temp_bitmap2, def_bb_insn->index) /* does not reach use_bb */
        || !bitmap_empty_p (temp_bitmap2))                     /* another def reaches use_bb */
      return FALSE;
  }
  return TRUE;
}

static int another_def_reaching_def_p (MIR_context_t ctx, bb_insn_t def_bb_insn, MIR_reg_t var) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t ignore_var;
  bitmap_iterator_t bi;
  bb_insn_t another_def_bb_insn;
  bb_t another_def_bb, def_bb = def_bb_insn->bb;
  int out_p, mem_p;
  size_t nbit, passed_mem_num, n_out;
  insn_var_iterator_t iter;

  bitmap_and (temp_bitmap, get_var_uses_or_defs (ctx, var, FALSE), loop_defs_bitmap);
  bitmap_and (temp_bitmap2, def_bb->rdef_in, temp_bitmap);
  n_out = 0;
  FOREACH_INSN_VAR (ctx, iter, def_bb_insn->insn, ignore_var, out_p, mem_p, passed_mem_num) {
    if (!out_p) continue;
    bitmap_clear_bit_p (temp_bitmap2, def_bb_insn->index + n_out++); /* exclude insn defs itself */
  }
  if (!bitmap_empty_p (temp_bitmap2)) return TRUE; /* another def reaches def bb */
  FOREACH_BITMAP_BIT (bi, temp_bitmap, nbit) {     /* check defs in the same BB: */
    if ((another_def_bb_insn = VARR_GET (bb_insn_t, def_bb_insns, nbit)) == def_bb_insn) continue;
    another_def_bb = another_def_bb_insn->bb;
    if (another_def_bb == def_bb && another_def_bb_insn->index < def_bb_insn->index) return TRUE;
  }
  return FALSE;
}

static int loop_invariant_insn_p (MIR_context_t ctx, loop_node_t loop, bb_insn_t bb_insn,
                                  size_t stores_start) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn = bb_insn->insn;
  bb_t bb = bb_insn->bb;
  size_t i, passed_mem_num;
  int out_p, mem_p;
  MIR_reg_t var;
  bitmap_t var_rdef_bitmap = temp_bitmap;
  insn_var_iterator_t iter;

  if (MIR_branch_code_p (insn->code) || insn->code == MIR_RET || insn->code == MIR_SWITCH
      || insn->code == MIR_LABEL || MIR_call_code_p (insn->code) || insn->code == MIR_ALLOCA
      || insn->code == MIR_BSTART || insn->code == MIR_BEND || insn->code == MIR_VA_START
      || insn->code == MIR_VA_ARG || insn->code == MIR_VA_END)
    return FALSE;
  if (!dominate_curr_loop_exits_p (ctx, bb)) return FALSE; /* can not safely move */
  FOREACH_INSN_VAR (ctx, iter, insn, var, out_p, mem_p, passed_mem_num) {
    /* no stores in the loop ??? aliasing should be used here */
    if (passed_mem_num != 0
        && (loop->call_p || stores_start != VARR_LENGTH (bb_insn_t, loop_stores)))
      return FALSE;
    if (!out_p) {
      /* in var: check reaching defs of var is outside the loop or loop invariant */
      bitmap_and (var_rdef_bitmap, curr_rdef_bitmap, get_var_uses_or_defs (ctx, var, FALSE));
      if (!invariant_reaching_definitions_p (ctx, loop, var, var_rdef_bitmap)) return FALSE;
    } else { /* out var: check all uses in the loop are dominated by the def (bb_insn) */
      if (!dominate_uses_p (ctx, bb_insn, var)) return FALSE;
      if (another_def_reaching_def_p (ctx, bb_insn, var)) return FALSE;
    }
  }
  return TRUE;
}

static int collect_loop_bb_invariants (MIR_context_t ctx, loop_node_t loop, bb_t bb,
                                       size_t stores_start) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bitmap_t def_bitmap;
  MIR_reg_t var;
  int out_p, mem_p, change_p = FALSE;
  size_t passed_mem_num, n_out;
  insn_var_iterator_t iter;

  bitmap_copy (curr_rdef_bitmap, bb->rdef_in);
  for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
       bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
    if (!bb_insn->flag && loop_invariant_insn_p (ctx, loop, bb_insn, stores_start)) {
      bb_insn->flag = TRUE;
      VARR_PUSH (bb_insn_t, invariant_bb_insns, bb_insn);
      change_p = TRUE;
    }
    /* Update curr_rdef_bitmap: */
    n_out = 0;
    FOREACH_INSN_VAR (ctx, iter, bb_insn->insn, var, out_p, mem_p, passed_mem_num) {
      if (!out_p) continue;
      def_bitmap = get_var_uses_or_defs (ctx, var, FALSE);
      gen_assert (def_bitmap != NULL);
      bitmap_and_compl (curr_rdef_bitmap, curr_rdef_bitmap, def_bitmap);
      bitmap_set_bit_p (curr_rdef_bitmap, bb_insn->index + n_out++);
    }
  }
  return change_p;
}

static int compare_bb_bfs (const void *p1, const void *p2) {
  const bb_t *bb1 = (const bb_t *) p1, *bb2 = (const bb_t *) p2;

  return (*bb1)->bfs < (*bb2)->bfs ? -1 : (*bb1)->bfs > (*bb2)->bfs ? 1 : 0;
}

static int int_var_type_p (MIR_context_t ctx, MIR_reg_t var) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (!var_is_reg_p (var)) return target_hard_reg_type_ok_p (var, MIR_T_I32);
  return MIR_int_type_p (MIR_reg_type (ctx, var2reg (gen_ctx, var), curr_func_item->u.func));
}

static void find_profitable_loop_invariants (MIR_context_t ctx, loop_node_t loop) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_insn_t bb_insn;
  MIR_insn_t insn;
  MIR_op_mode_t mode;
  MIR_reg_t var;
  int out_p, mem_p, move_p, dep_p, int_p;
  int int_pressure, fp_pressure;
  size_t passed_mem_num;
  insn_var_iterator_t iterator;
  bitmap_t live = temp_bitmap;

  bitmap_clear (live);
  for (size_t i = VARR_LENGTH (bb_insn_t, invariant_bb_insns); i != 0;) {
    i--;
    bb_insn = VARR_GET (bb_insn_t, invariant_bb_insns, i);
    gen_assert (bb_insn->flag);
    insn = bb_insn->insn;
    move_p = !(insn->code == MIR_MOV
               && ((mode = insn->ops[1].mode) == MIR_OP_INT || mode == MIR_OP_UINT
                   || mode == MIR_OP_REG || mode == MIR_OP_HARD_REG)); /* ??? */
    dep_p = FALSE;
    int_pressure = loop->max_int_pressure;
    fp_pressure = loop->max_fp_pressure;
    bb_insn->flag2 = TRUE; /* cheap or pressure */
    FOREACH_INSN_VAR (ctx, iterator, insn, var, out_p, mem_p, passed_mem_num) {
      if (!out_p) continue;
      if (bitmap_bit_p (live, var)) {
        move_p = dep_p = TRUE;
        break;
      }
      /* inaccurate reg pressure evaluation: */
      if ((int_p = int_var_type_p (ctx, var))) {
        if (++int_pressure >= max_int_hard_regs) bb_insn->flag2 = move_p = FALSE;
      } else {
        if (++fp_pressure >= max_fp_hard_regs) bb_insn->flag2 = move_p = FALSE;
      }
      int_p ? int_pressure++ : fp_pressure++;
    }
    if (move_p || dep_p) { /* update live: ignore killed vars here */
      bb_insn->flag = FALSE;
      FOREACH_INSN_VAR (ctx, iterator, insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p) bitmap_set_bit_p (live, var);
      }
    }
  }
}

static void licm_move_insn (MIR_context_t ctx, bb_insn_t bb_insn, bb_t to, bb_insn_t before) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_t bb = bb_insn->bb;
  MIR_insn_t insn = bb_insn->insn;

  gen_assert (before != NULL);
  DLIST_REMOVE (bb_insn_t, bb->bb_insns, bb_insn);
  DLIST_APPEND (bb_insn_t, to->bb_insns, bb_insn);
  bb_insn->bb = to;
  DLIST_REMOVE (MIR_insn_t, curr_func_item->u.func->insns, insn);
  MIR_insert_insn_before (ctx, curr_func_item, before->insn, insn);
}

static void move_loop_invariants (MIR_context_t ctx, loop_node_t loop) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_t entry_bb = loop->entry->bb;

  find_profitable_loop_invariants (ctx, loop);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL)
    fprintf (debug_file, "Loop%3lu (pressure: int=%d, fp=%d) -- invariant insns:\n",
             (unsigned long) loop->index, loop->max_int_pressure, loop->max_fp_pressure);
#endif
  for (size_t i = 0; i < VARR_LENGTH (bb_insn_t, invariant_bb_insns); i++) {
    bb_insn_t bb_insn = VARR_GET (bb_insn_t, invariant_bb_insns, i);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "  %15s %-5lu",
               !bb_insn->flag ? "move" : bb_insn->flag2 ? "keep cheap" : "keep pressure",
               (unsigned long) bb_insn->index);
      print_bb_insn (ctx, bb_insn, TRUE);
    }
#endif
    if (!bb_insn->flag) {
      gen_assert (entry_bb != NULL);
      licm_move_insn (ctx, bb_insn, loop->preheader, DLIST_HEAD (bb_insn_t, entry_bb->bb_insns));
    }
  }
}

static int store_p (MIR_insn_t insn) {
  MIR_insn_code_t code = insn->code;

  if (code != MIR_MOV && code != MIR_DMOV && code != MIR_FMOV && code != MIR_LDMOV) return FALSE;
  return insn->ops[0].mode == MIR_OP_MEM || insn->ops[0].mode == MIR_OP_HARD_REG_MEM;
}

static void loop_licm (MIR_context_t ctx, loop_node_t loop) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t var;
  size_t i, n_out, passed_mem_num;
  int out_p, mem_p, change_p;
  bb_t bb;
  insn_var_iterator_t iter;
  size_t loop_bbs_start = VARR_LENGTH (bb_t, loop_bbs);
  size_t stores_start = VARR_LENGTH (bb_insn_t, loop_stores);

  for (loop_node_t node = DLIST_HEAD (loop_node_t, loop->children); node != NULL;
       node = DLIST_NEXT (loop_node_t, node))
    if (node->bb == NULL) { /* process sub-loops first */
      loop_licm (ctx, node);
    } else {
      VARR_PUSH (bb_t, loop_bbs, node->bb); /* collect loop bbs */
    }
  if (loop == curr_cfg->root_loop_node || loop->preheader == NULL) return;
  VARR_TRUNC (bb_t, curr_exit_loop_bbs, 0);
  bitmap_clear (loop_defs_bitmap);
  loop->call_p = TRUE;
  for (i = loop_bbs_start; i < VARR_LENGTH (bb_t, loop_bbs); i++) {
    bb = VARR_GET (bb_t, loop_bbs, i);
    if (bb_loop_exit_p (ctx, bb, loop)) VARR_PUSH (bb_t, curr_exit_loop_bbs, bb);
    for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
      if (store_p (bb_insn->insn)) VARR_PUSH (bb_insn_t, loop_stores, bb_insn);
      if (MIR_call_code_p (bb_insn->insn->code)) loop->call_p = TRUE;
      bb_insn->flag = FALSE; /* clear invariant flag */
      n_out = 0;
      FOREACH_INSN_VAR (ctx, iter, bb_insn->insn, var, out_p, mem_p, passed_mem_num) {
        if (out_p) bitmap_set_bit_p (loop_defs_bitmap, bb_insn->index + n_out++);
      }
    }
  }
  qsort (VARR_ADDR (bb_t, loop_bbs) + loop_bbs_start, VARR_LENGTH (bb_t, loop_bbs) - loop_bbs_start,
         sizeof (bb_t), compare_bb_bfs);
  VARR_TRUNC (bb_insn_t, invariant_bb_insns, 0);
  do {
    change_p = FALSE;
    for (i = loop_bbs_start; i < VARR_LENGTH (bb_t, loop_bbs); i++) {
      bb = VARR_GET (bb_t, loop_bbs, i);
      if (collect_loop_bb_invariants (ctx, loop, bb, stores_start)) change_p = TRUE;
    }
  } while (change_p);
  move_loop_invariants (ctx, loop);
}

static void BFS (MIR_context_t ctx) { /* breadth-first search */
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_t bb, dst;
  edge_t e;
  size_t bfs = 0;

  VARR_TRUNC (bb_t, licm_temp_bbs, 0);
  VARR_PUSH (bb_t, licm_temp_bbs, DLIST_HEAD (bb_t, curr_cfg->bbs)); /* entry */
  while (VARR_LENGTH (bb_t, licm_temp_bbs) != 0) {
    bb = VARR_POP (bb_t, licm_temp_bbs);
    bb->bfs = bfs++;
    for (e = DLIST_TAIL (out_edge_t, bb->out_edges); e != NULL; e = DLIST_PREV (out_edge_t, e))
      if ((dst = e->dst)->bfs == 0) VARR_PUSH (bb_t, licm_temp_bbs, dst);
    for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_NEXT (out_edge_t, e))
      if ((dst = e->dst)->bfs == 0) dst->bfs = bfs++;
  }
}

static int licm (MIR_context_t ctx) { /* loop invariant code motion */
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) print_loop_tree (ctx, FALSE);
#endif
  BFS (ctx);
  calculate_dominators (ctx);
  VARR_TRUNC (bb_t, licm_temp_bbs, 0);
  VARR_TRUNC (bb_t, loop_bbs, 0);
  VARR_TRUNC (bb_insn_t, loop_stores, 0);
  loop_licm (ctx, curr_cfg->root_loop_node);
  return TRUE;
}

#if !MIR_NO_GEN_DEBUG
static void output_bb_licm_info (MIR_context_t ctx, bb_t bb) {
  output_bitmap (ctx, "  dom_in:", bb->dom_in, FALSE);
  output_bitmap (ctx, "  dom_out:", bb->dom_out, FALSE);
  output_bb_rdef_info (ctx, bb);
}
#endif

static void init_licm (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->licm_ctx = gen_malloc (ctx, sizeof (struct licm_ctx));
  VARR_CREATE (bb_t, licm_temp_bbs, 0);
  VARR_CREATE (bb_t, loop_bbs, 0);
  VARR_CREATE (bb_insn_t, loop_stores, 0);
  VARR_CREATE (bb_t, curr_exit_loop_bbs, 0);
  VARR_CREATE (bb_insn_t, invariant_bb_insns, 0);
  loop_defs_bitmap = bitmap_create ();
}

static void finish_licm (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (bb_t, licm_temp_bbs);
  VARR_DESTROY (bb_t, loop_bbs);
  VARR_DESTROY (bb_insn_t, loop_stores);
  VARR_DESTROY (bb_t, curr_exit_loop_bbs);
  VARR_DESTROY (bb_insn_t, invariant_bb_insns);
  bitmap_destroy (loop_defs_bitmap);
  free (gen_ctx->licm_ctx);
  gen_ctx->licm_ctx = NULL;
}

#undef rdef_in
#undef rdef_out
#undef rdef_kill
#undef rdef_gen

/* New Page */

/* Sparse Conditional Constant Propagation.  Live info should exist.  */

#define live_in in
#define live_out out

enum ccp_val_kind { CCP_CONST = 0, CCP_VARYING, CCP_UNKNOWN };

enum place_type { OCC_INSN, OCC_BB_START, OCC_BB_END };

typedef struct {
  enum place_type type;
  union {
    MIR_insn_t insn;
    bb_t bb;
  } u;
} place_t;

typedef struct var_occ *var_occ_t;

DEF_DLIST_LINK (var_occ_t);
DEF_DLIST_TYPE (var_occ_t);

/* Occurences at BB start are defs, ones at BB end are uses.  */
struct var_occ {
  MIR_reg_t var;
  enum ccp_val_kind val_kind : 8;
  unsigned int flag : 8;
  const_t val;
  place_t place;
  var_occ_t def;
  DLIST (var_occ_t) uses; /* Empty for def */
  DLIST_LINK (var_occ_t) use_link;
};

DEF_DLIST_CODE (var_occ_t, use_link);

typedef DLIST (var_occ_t) bb_start_occ_list_t;
DEF_VARR (bb_start_occ_list_t);

DEF_VARR (var_occ_t);
DEF_HTAB (var_occ_t);

typedef struct {
  int producer_age, op_age;
  var_occ_t producer;   /* valid if producer_age == curr_producer_age */
  var_occ_t op_var_use; /* valid if op_age == curr_op_age */
} var_producer_t;

DEF_VARR (var_producer_t);

struct ccp_ctx {
  VARR (bb_start_occ_list_t) * bb_start_occ_list_varr;
  bb_start_occ_list_t *bb_start_occ_lists;
  VARR (var_occ_t) * var_occs;
  HTAB (var_occ_t) * var_occ_tab;
  int curr_producer_age, curr_op_age;
  var_producer_t *producers;
  VARR (var_producer_t) * producer_varr;
  bitmap_t bb_visited;
  VARR (bb_t) * ccp_bbs;
  VARR (var_occ_t) * ccp_var_occs;
  VARR (bb_insn_t) * ccp_insns;
};

#define bb_start_occ_list_varr gen_ctx->ccp_ctx->bb_start_occ_list_varr
#define bb_start_occ_lists gen_ctx->ccp_ctx->bb_start_occ_lists
#define var_occs gen_ctx->ccp_ctx->var_occs
#define var_occ_tab gen_ctx->ccp_ctx->var_occ_tab
#define curr_producer_age gen_ctx->ccp_ctx->curr_producer_age
#define curr_op_age gen_ctx->ccp_ctx->curr_op_age
#define producers gen_ctx->ccp_ctx->producers
#define producer_varr gen_ctx->ccp_ctx->producer_varr
#define bb_visited gen_ctx->ccp_ctx->bb_visited
#define ccp_bbs gen_ctx->ccp_ctx->ccp_bbs
#define ccp_var_occs gen_ctx->ccp_ctx->ccp_var_occs
#define ccp_insns gen_ctx->ccp_ctx->ccp_insns

static htab_hash_t var_occ_hash (var_occ_t vo, void *arg) {
  gen_assert (vo->place.type != OCC_INSN);
  return mir_hash_finish (
    mir_hash_step (mir_hash_step (mir_hash_step (mir_hash_init (0x54), (uint64_t) vo->var),
                                  (uint64_t) vo->place.type),
                   (uint64_t) vo->place.u.bb));
}

static int var_occ_eq (var_occ_t vo1, var_occ_t vo2, void *arg) {
  return (vo1->var == vo2->var && vo1->place.type == vo2->place.type
          && vo1->place.u.bb == vo2->place.u.bb);
}

static void init_var_occ (var_occ_t var_occ, MIR_reg_t var, enum place_type type, bb_t bb,
                          MIR_insn_t insn) {
  var_occ->var = var;
  var_occ->val_kind = CCP_UNKNOWN;
  var_occ->place.type = type;
  if (bb == NULL)
    var_occ->place.u.insn = insn;
  else
    var_occ->place.u.bb = bb;
  var_occ->def = NULL;
  var_occ->flag = FALSE;
  DLIST_INIT (var_occ_t, var_occ->uses);
}

static var_occ_t new_insn_var_occ (MIR_context_t ctx, MIR_reg_t var, MIR_insn_t insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  var_occ_t var_occ = gen_malloc (ctx, sizeof (struct var_occ));

  init_var_occ (var_occ, var, OCC_INSN, NULL, insn);
  VARR_PUSH (var_occ_t, var_occs, var_occ);
  return var_occ;
}

static var_occ_t get_bb_var_occ (MIR_context_t ctx, MIR_reg_t var, enum place_type type, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  struct var_occ vos;
  var_occ_t var_occ;

  init_var_occ (&vos, var, type, bb, NULL);
  if (HTAB_DO (var_occ_t, var_occ_tab, &vos, HTAB_FIND, var_occ)) return var_occ;
  var_occ = gen_malloc (ctx, sizeof (struct var_occ));
  *var_occ = vos;
  VARR_PUSH (var_occ_t, var_occs, var_occ);
  HTAB_DO (var_occ_t, var_occ_tab, var_occ, HTAB_INSERT, var_occ);
  if (type == OCC_BB_START) {
    DLIST_APPEND (var_occ_t, bb_start_occ_lists[bb->index], var_occ);
    if (DLIST_EL (bb_t, curr_cfg->bbs, 0) == bb && var_is_reg_p (var)
        && var2reg (gen_ctx, var) <= curr_func_item->u.func->nargs) {
      var_occ->val_kind = CCP_VARYING;
    }
  }
  return var_occ;
}

static var_occ_t get_var_def (MIR_context_t ctx, MIR_reg_t var, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  var_occ_t var_occ;

  if (producers[var].producer_age == curr_producer_age) {
    var_occ = producers[var].producer;
  } else { /* use w/o a producer insn in the block */
    producers[var].producer = var_occ = get_bb_var_occ (ctx, var, OCC_BB_START, bb);
    producers[var].producer_age = curr_producer_age;
  }
  return var_occ;
}

static void process_op_var_use (MIR_context_t ctx, MIR_reg_t var, bb_insn_t bb_insn, MIR_op_t *op) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  var_occ_t def, use;

  if (producers[var].op_age == curr_op_age) {
    op->data = producers[var].op_var_use;
    return; /* var was already another operand in the insn */
  }
  producers[var].op_age = curr_op_age;
  def = get_var_def (ctx, var, bb_insn->bb);
  producers[var].op_var_use = use = new_insn_var_occ (ctx, var, bb_insn->insn);
  op->data = use;
  use->def = def;
  DLIST_APPEND (var_occ_t, def->uses, use);
}

static void process_op_use (MIR_context_t ctx, MIR_op_t *op, bb_insn_t bb_insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  switch (op->mode) {
  case MIR_OP_REG:
    if (op->u.reg != 0) process_op_var_use (ctx, reg2var (gen_ctx, op->u.reg), bb_insn, op);
    break;
  case MIR_OP_HARD_REG:
    if (op->u.hard_reg != MIR_NON_HARD_REG) process_op_var_use (ctx, op->u.hard_reg, bb_insn, op);
    break;
  case MIR_OP_MEM:
    if (op->u.mem.base != 0)
      process_op_var_use (ctx, reg2var (gen_ctx, op->u.mem.base), bb_insn, op);
    if (op->u.mem.index != 0)
      process_op_var_use (ctx, reg2var (gen_ctx, op->u.mem.index), bb_insn, op);
    break;
  case MIR_OP_HARD_REG_MEM:
    if (op->u.hard_reg_mem.base != MIR_NON_HARD_REG)
      process_op_var_use (ctx, op->u.hard_reg_mem.base, bb_insn, op);
    if (op->u.hard_reg_mem.index != MIR_NON_HARD_REG)
      process_op_var_use (ctx, op->u.hard_reg_mem.index, bb_insn, op);
    break;
  default: break;
  }
}

/* Build a web of def-use with auxiliary usages and definitions at BB
   borders to emulate SSA on which the sparse conditional propagation
   is usually done.  We could do non-sparse CCP w/o building the web
   but it is much slower algorithm.  */
static void build_var_occ_web (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_op_t *op;
  MIR_insn_t insn;
  size_t i, nops, nel;
  int out_p;
  MIR_reg_t dst_var;
  var_occ_t var_occ;
  var_producer_t var_producer;
  bb_start_occ_list_t list;
  bitmap_iterator_t bi;

  DLIST_INIT (var_occ_t, list);
  while (VARR_LENGTH (bb_start_occ_list_t, bb_start_occ_list_varr) < curr_bb_index)
    VARR_PUSH (bb_start_occ_list_t, bb_start_occ_list_varr, list);
  bb_start_occ_lists = VARR_ADDR (bb_start_occ_list_t, bb_start_occ_list_varr);
  var_producer.producer_age = var_producer.op_age = 0;
  var_producer.producer = var_producer.op_var_use = NULL;
  while (VARR_LENGTH (var_producer_t, producer_varr) < get_nvars (ctx))
    VARR_PUSH (var_producer_t, producer_varr, var_producer);
  producers = VARR_ADDR (var_producer_t, producer_varr);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    curr_producer_age++;
    for (bb_insn_t bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
      curr_op_age++;
      insn = bb_insn->insn;
      nops = MIR_insn_nops (ctx, insn);
      for (i = 0; i < nops; i++) { /* process inputs */
        MIR_insn_op_mode (ctx, insn, i, &out_p);
        op = &insn->ops[i];
        if (!out_p) process_op_use (ctx, op, bb_insn);
      }
      for (i = 0; i < nops; i++) { /* process outputs */
        MIR_insn_op_mode (ctx, insn, i, &out_p);
        op = &insn->ops[i];
        if (out_p && (op->mode == MIR_OP_REG || op->mode == MIR_OP_HARD_REG)) {
          dst_var = op->mode == MIR_OP_HARD_REG ? op->u.hard_reg : reg2var (gen_ctx, op->u.reg);
          producers[dst_var].producer_age = curr_producer_age;
          producers[dst_var].op_age = curr_op_age;
          producers[dst_var].producer = var_occ = new_insn_var_occ (ctx, dst_var, insn);
          op->data = producers[dst_var].producer;
        }
      }
    }
    FOREACH_BITMAP_BIT (bi, bb->live_out, nel) {
      MIR_reg_t var = nel;
      var_occ_t use = get_bb_var_occ (ctx, var, OCC_BB_END, bb);
      var_occ_t def = get_var_def (ctx, var, bb);

      use->def = def;
      DLIST_APPEND (var_occ_t, def->uses, use);
    }
  }
}

#undef live_in
#undef live_out

static void var_occs_clear (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  HTAB_CLEAR (var_occ_t, var_occ_tab);
  while (VARR_LENGTH (var_occ_t, var_occs) != 0) free (VARR_POP (var_occ_t, var_occs));
  VARR_TRUNC (bb_start_occ_list_t, bb_start_occ_list_varr, 0);
  VARR_TRUNC (var_producer_t, producer_varr, 0);
}

static void init_var_occs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_CREATE (bb_start_occ_list_t, bb_start_occ_list_varr, 256);
  VARR_CREATE (var_occ_t, var_occs, 1024);
  curr_producer_age = curr_op_age = 0;
  VARR_CREATE (var_producer_t, producer_varr, 256);
  HTAB_CREATE (var_occ_t, var_occ_tab, 1024, var_occ_hash, var_occ_eq, NULL);
}

static void finish_var_occs (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (bb_start_occ_list_t, bb_start_occ_list_varr);
  VARR_DESTROY (var_occ_t, var_occs);
  VARR_DESTROY (var_producer_t, producer_varr);
  HTAB_DESTROY (var_occ_t, var_occ_tab);
}

static void initiate_ccp_info (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_insn_t bb_insn;

  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    if ((bb_insn = DLIST_TAIL (bb_insn_t, bb->bb_insns)) != NULL
        && MIR_branch_code_p (bb_insn->insn->code) && bb_insn->insn->code != MIR_JMP
        && bb_insn->insn->code != MIR_SWITCH) {
      for (edge_t e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL;
           e = DLIST_NEXT (out_edge_t, e))
        if (e->dst != DLIST_EL (bb_t, curr_cfg->bbs, 1)) /* ignore exit bb */
          e->skipped_p = TRUE;
    }
  }
  bitmap_clear (bb_visited);
  VARR_TRUNC (var_occ_t, ccp_var_occs, 0);
  VARR_TRUNC (bb_insn_t, ccp_insns, 0);
  VARR_TRUNC (bb_t, ccp_bbs, 0);
  VARR_PUSH (bb_t, ccp_bbs, DLIST_HEAD (bb_t, curr_cfg->bbs)); /* entry bb */
}

static int var_op_p (MIR_op_t op) { return op.mode == MIR_OP_HARD_REG || op.mode == MIR_OP_REG; }
static int var_insn_op_p (MIR_insn_t insn, size_t nop) { return var_op_p (insn->ops[nop]); }

static enum ccp_val_kind get_op (MIR_insn_t insn, size_t nop, const_t *val) {
  MIR_op_t op;
  var_occ_t var_occ, def;

  if (!var_insn_op_p (insn, nop)) {
    if ((op = insn->ops[nop]).mode == MIR_OP_INT) {
      val->uns_p = FALSE;
      val->u.i = op.u.i;
      return CCP_CONST;
    } else if (op.mode == MIR_OP_UINT) {
      val->uns_p = TRUE;
      val->u.u = op.u.u;
      return CCP_CONST;
    }
    return CCP_VARYING;
  }
  var_occ = insn->ops[nop].data;
  def = var_occ->def;
  if (def->val_kind == CCP_CONST) *val = def->val;
  return def->val_kind;
}

static enum ccp_val_kind get_2ops (MIR_insn_t insn, const_t *val1, int out_p) {
  if (out_p && !var_insn_op_p (insn, 0)) return CCP_UNKNOWN;
  return get_op (insn, 1, val1);
}

static enum ccp_val_kind get_3ops (MIR_insn_t insn, const_t *val1, const_t *val2, int out_p) {
  enum ccp_val_kind res1, res2;

  if (out_p && !var_insn_op_p (insn, 0)) return CCP_UNKNOWN;
  if ((res1 = get_op (insn, 1, val1)) == CCP_VARYING) return CCP_VARYING;
  if ((res2 = get_op (insn, 2, val2)) == CCP_VARYING) return CCP_VARYING;
  return res1 == CCP_UNKNOWN || res2 == CCP_UNKNOWN ? CCP_UNKNOWN : CCP_CONST;
}

static enum ccp_val_kind get_2iops (MIR_insn_t insn, int64_t *p, int out_p) {
  const_t val;
  enum ccp_val_kind res;

  if ((res = get_2ops (insn, &val, out_p))) return res;
  *p = val.u.i;
  return CCP_CONST;
}

static enum ccp_val_kind get_2isops (MIR_insn_t insn, int32_t *p, int out_p) {
  const_t val;
  enum ccp_val_kind res;

  if ((res = get_2ops (insn, &val, out_p))) return res;
  *p = val.u.i;
  return CCP_CONST;
}

static enum ccp_val_kind MIR_UNUSED get_2usops (MIR_insn_t insn, uint32_t *p, int out_p) {
  const_t val;
  enum ccp_val_kind res;

  if ((res = get_2ops (insn, &val, out_p))) return res;
  *p = val.u.u;
  return CCP_CONST;
}

static enum ccp_val_kind get_3iops (MIR_insn_t insn, int64_t *p1, int64_t *p2, int out_p) {
  const_t val1, val2;
  enum ccp_val_kind res;

  if ((res = get_3ops (insn, &val1, &val2, out_p))) return res;
  *p1 = val1.u.i;
  *p2 = val2.u.i;
  return CCP_CONST;
}

static enum ccp_val_kind get_3isops (MIR_insn_t insn, int32_t *p1, int32_t *p2, int out_p) {
  const_t val1, val2;
  enum ccp_val_kind res;

  if ((res = get_3ops (insn, &val1, &val2, out_p))) return res;
  *p1 = val1.u.i;
  *p2 = val2.u.i;
  return CCP_CONST;
}

static enum ccp_val_kind get_3uops (MIR_insn_t insn, uint64_t *p1, uint64_t *p2, int out_p) {
  const_t val1, val2;
  enum ccp_val_kind res;

  if ((res = get_3ops (insn, &val1, &val2, out_p))) return res;
  *p1 = val1.u.u;
  *p2 = val2.u.u;
  return CCP_CONST;
}

static enum ccp_val_kind get_3usops (MIR_insn_t insn, uint32_t *p1, uint32_t *p2, int out_p) {
  const_t val1, val2;
  enum ccp_val_kind res;

  if ((res = get_3ops (insn, &val1, &val2, out_p))) return res;
  *p1 = val1.u.u;
  *p2 = val2.u.u;
  return CCP_CONST;
}

#define EXT(tp)                                                              \
  do {                                                                       \
    int64_t p;                                                               \
    if ((ccp_res = get_2iops (insn, &p, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                       \
    val.u.i = (tp) p;                                                        \
  } while (0)

#define IOP2(op)                                                             \
  do {                                                                       \
    int64_t p;                                                               \
    if ((ccp_res = get_2iops (insn, &p, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                       \
    val.u.i = op p;                                                          \
  } while (0)

#define IOP2S(op)                                                             \
  do {                                                                        \
    int32_t p;                                                                \
    if ((ccp_res = get_2isops (insn, &p, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                        \
    val.u.i = op p;                                                           \
  } while (0)

#define UOP2S(op)                                                             \
  do {                                                                        \
    uint32_t p;                                                               \
    if ((ccp_res = get_2usops (insn, &p, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                        \
    val.u.u = op p;                                                           \
  } while (0)

#define IOP3(op)                                                                   \
  do {                                                                             \
    int64_t p1, p2;                                                                \
    if ((ccp_res = get_3iops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                             \
    val.u.i = p1 op p2;                                                            \
  } while (0)

#define IOP3S(op)                                                                   \
  do {                                                                              \
    int32_t p1, p2;                                                                 \
    if ((ccp_res = get_3isops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                              \
    val.u.i = p1 op p2;                                                             \
  } while (0)

#define UOP3(op)                                                                   \
  do {                                                                             \
    uint64_t p1, p2;                                                               \
    if ((ccp_res = get_3uops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = TRUE;                                                              \
    val.u.u = p1 op p2;                                                            \
  } while (0)

#define UOP3S(op)                                                                   \
  do {                                                                              \
    uint32_t p1, p2;                                                                \
    if ((ccp_res = get_3usops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = TRUE;                                                               \
    val.u.u = p1 op p2;                                                             \
  } while (0)

#define IOP30(op)                                                                         \
  do {                                                                                    \
    if ((ccp_res = get_op (insn, 2, &val)) != CCP_CONST || val.u.i == 0) goto non_const0; \
    IOP3 (op);                                                                            \
  } while (0)

#define IOP3S0(op)                                                                        \
  do {                                                                                    \
    if ((ccp_res = get_op (insn, 2, &val)) != CCP_CONST || val.u.i == 0) goto non_const0; \
    IOP3S (op);                                                                           \
  } while (0)

#define UOP30(op)                                                                         \
  do {                                                                                    \
    if ((ccp_res = get_op (insn, 2, &val)) != CCP_CONST || val.u.u == 0) goto non_const0; \
    UOP3 (op);                                                                            \
  } while (0)

#define UOP3S0(op)                                                                        \
  do {                                                                                    \
    if ((ccp_res = get_op (insn, 2, &val)) != CCP_CONST || val.u.u == 0) goto non_const0; \
    UOP3S (op);                                                                           \
  } while (0)

#define ICMP(op)                                                                   \
  do {                                                                             \
    int64_t p1, p2;                                                                \
    if ((ccp_res = get_3iops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                             \
    val.u.i = p1 op p2;                                                            \
  } while (0)

#define ICMPS(op)                                                                   \
  do {                                                                              \
    int32_t p1, p2;                                                                 \
    if ((ccp_res = get_3isops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                              \
    val.u.i = p1 op p2;                                                             \
  } while (0)

#define UCMP(op)                                                                   \
  do {                                                                             \
    uint64_t p1, p2;                                                               \
    if ((ccp_res = get_3uops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                             \
    val.u.i = p1 op p2;                                                            \
  } while (0)

#define UCMPS(op)                                                                   \
  do {                                                                              \
    uint32_t p1, p2;                                                                \
    if ((ccp_res = get_3usops (insn, &p1, &p2, TRUE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                              \
    val.u.i = p1 op p2;                                                             \
  } while (0)

#define BICMP(op)                                                                   \
  do {                                                                              \
    int64_t p1, p2;                                                                 \
    if ((ccp_res = get_3iops (insn, &p1, &p2, FALSE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                              \
    val.u.i = p1 op p2;                                                             \
  } while (0)

#define BICMPS(op)                                                                   \
  do {                                                                               \
    int32_t p1, p2;                                                                  \
    if ((ccp_res = get_3isops (insn, &p1, &p2, FALSE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                               \
    val.u.i = p1 op p2;                                                              \
  } while (0)

#define BUCMP(op)                                                                   \
  do {                                                                              \
    uint64_t p1, p2;                                                                \
    if ((ccp_res = get_3uops (insn, &p1, &p2, FALSE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                              \
    val.u.i = p1 op p2;                                                             \
  } while (0)

#define BUCMPS(op)                                                                   \
  do {                                                                               \
    uint32_t p1, p2;                                                                 \
    if ((ccp_res = get_3usops (insn, &p1, &p2, FALSE)) != CCP_CONST) goto non_const; \
    val.uns_p = FALSE;                                                               \
    val.u.i = p1 op p2;                                                              \
  } while (0)

static int get_ccp_res_op (MIR_context_t ctx, MIR_insn_t insn, int out_num, MIR_op_t *op) {
  int out_p;
  MIR_op_t proto_op;
  MIR_proto_t proto;

  if (MIR_call_code_p (insn->code)) {
    proto_op = insn->ops[0];
    mir_assert (proto_op.mode == MIR_OP_REF && proto_op.u.ref->item_type == MIR_proto_item);
    proto = proto_op.u.ref->u.proto;
    if (out_num >= proto->nres) return FALSE;
    *op = insn->ops[out_num + 2];
    return TRUE;
  }
  if (out_num > 0 || MIR_insn_nops (ctx, insn) < 1) return FALSE;
  MIR_insn_op_mode (ctx, insn, 0, &out_p);
  if (!out_p) return FALSE;
  *op = insn->ops[0];
  return TRUE;
}

static int ccp_insn_update (MIR_context_t ctx, MIR_insn_t insn, const_t *res) {
  // ??? should we do CCP for FP too
  MIR_op_t op;
  int change_p;
  enum ccp_val_kind ccp_res;
  const_t val;
  var_occ_t var_occ;
  enum ccp_val_kind val_kind;

  switch (insn->code) {
  case MIR_MOV: IOP2 (+); break;
  case MIR_EXT8: EXT (int8_t); break;
  case MIR_EXT16: EXT (int16_t); break;
  case MIR_EXT32: EXT (int32_t); break;
  case MIR_UEXT8: EXT (uint8_t); break;
  case MIR_UEXT16: EXT (uint16_t); break;
  case MIR_UEXT32: EXT (uint32_t); break;

  case MIR_NEG: IOP2 (-); break;
  case MIR_NEGS: IOP2S (-); break;

  case MIR_ADD: IOP3 (+); break;
  case MIR_ADDS: IOP3S (+); break;

  case MIR_SUB: IOP3 (-); break;
  case MIR_SUBS: IOP3S (-); break;

  case MIR_MUL: IOP3 (*); break;
  case MIR_MULS: IOP3S (*); break;

  case MIR_DIV: IOP30 (/); break;
  case MIR_DIVS: IOP3S0 (/); break;
  case MIR_UDIV: UOP30 (/); break;
  case MIR_UDIVS: UOP3S0 (/); break;

  case MIR_MOD: IOP30 (%); break;
  case MIR_MODS: IOP3S0 (%); break;
  case MIR_UMOD: UOP30 (%); break;
  case MIR_UMODS: UOP3S0 (%); break;

  case MIR_AND: IOP3 (&); break;
  case MIR_ANDS: IOP3S (&); break;
  case MIR_OR: IOP3 (|); break;
  case MIR_ORS: IOP3S (|); break;
  case MIR_XOR: IOP3 (^); break;
  case MIR_XORS: IOP3S (^); break;

  case MIR_LSH: IOP3 (<<); break;
  case MIR_LSHS: IOP3S (<<); break;
  case MIR_RSH: IOP3 (>>); break;
  case MIR_RSHS: IOP3S (>>); break;
  case MIR_URSH: UOP3 (>>); break;
  case MIR_URSHS: UOP3S (>>); break;

  case MIR_EQ: ICMP (==); break;
  case MIR_EQS: ICMPS (==); break;
  case MIR_NE: ICMP (!=); break;
  case MIR_NES: ICMPS (!=); break;

  case MIR_LT: ICMP (<); break;
  case MIR_LTS: ICMPS (<); break;
  case MIR_ULT: UCMP (<); break;
  case MIR_ULTS: UCMPS (<); break;
  case MIR_LE: ICMP (<=); break;
  case MIR_LES: ICMPS (<=); break;
  case MIR_ULE: UCMP (<=); break;
  case MIR_ULES: UCMPS (<=); break;
  case MIR_GT: ICMP (>); break;
  case MIR_GTS: ICMPS (>); break;
  case MIR_UGT: UCMP (>); break;
  case MIR_UGTS: UCMPS (>); break;
  case MIR_GE: ICMP (>=); break;
  case MIR_GES: ICMPS (>=); break;
  case MIR_UGE: UCMP (>=); break;
  case MIR_UGES: UCMPS (>=); break;

  default: ccp_res = CCP_VARYING; goto non_const;
  }
#ifndef NDEBUG
  {
    int out_p;

    MIR_insn_op_mode (ctx, insn, 0, &out_p); /* result here is always 0-th op */
    gen_assert (out_p);
  }
#endif
  var_occ = insn->ops[0].data;
  val_kind = var_occ->val_kind;
  gen_assert (var_occ->def == NULL && (val_kind == CCP_UNKNOWN || val_kind == CCP_CONST));
  var_occ->val_kind = CCP_CONST;
  var_occ->val = val;
  if (res != NULL) *res = val;
  return val_kind != CCP_CONST;
non_const0:
  if (ccp_res == CCP_CONST && val.u.i == 0) ccp_res = CCP_VARYING;
non_const:
  if (ccp_res == CCP_UNKNOWN) return FALSE;
  gen_assert (ccp_res == CCP_VARYING);
  change_p = FALSE;
  for (int i = 0; get_ccp_res_op (ctx, insn, i, &op); i++) {
    if (op.mode != MIR_OP_HARD_REG && op.mode != MIR_OP_REG) continue;
    var_occ = op.data;
    gen_assert (var_occ->def == NULL);
    if (var_occ->val_kind != CCP_VARYING) change_p = TRUE;
    var_occ->val_kind = CCP_VARYING;
  }
  return change_p;
}

static enum ccp_val_kind ccp_branch_update (MIR_insn_t insn, int *res) {
  enum ccp_val_kind ccp_res;
  const_t val;

  switch (insn->code) {
  case MIR_BT:
  case MIR_BTS:
  case MIR_BF:
  case MIR_BFS:
    if ((ccp_res = get_op (insn, 1, &val)) != CCP_CONST) return ccp_res;
    if (insn->code == MIR_BTS || insn->code == MIR_BFS)
      *res = val.uns_p ? (uint32_t) val.u.u != 0 : (int32_t) val.u.i != 0;
    else
      *res = val.uns_p ? val.u.u != 0 : val.u.i != 0;
    if (insn->code == MIR_BF || insn->code == MIR_BFS) *res = !*res;
    return CCP_CONST;
  case MIR_BEQ: BICMP (==); break;
  case MIR_BEQS: BICMPS (==); break;
  case MIR_BNE: BICMP (!=); break;
  case MIR_BNES: BICMPS (!=); break;

  case MIR_BLT: BICMP (<); break;
  case MIR_BLTS: BICMPS (<); break;
  case MIR_UBLT: BUCMP (<); break;
  case MIR_UBLTS: BUCMPS (<); break;
  case MIR_BLE: BICMP (<=); break;
  case MIR_BLES: BICMPS (<=); break;
  case MIR_UBLE: BUCMP (<=); break;
  case MIR_UBLES: BUCMPS (<=); break;
  case MIR_BGT: BICMP (>); break;
  case MIR_BGTS: BICMPS (>); break;
  case MIR_UBGT: BUCMP (>); break;
  case MIR_UBGTS: BUCMPS (>); break;
  case MIR_BGE: BICMP (>=); break;
  case MIR_BGES: BICMPS (>=); break;
  case MIR_UBGE: BUCMP (>=); break;
  case MIR_UBGES: BUCMPS (>=); break;

  default: return CCP_VARYING;  // ??? should we do CCP for FP BCMP too
  }
  *res = val.u.i;
  return CCP_CONST;
non_const:
  return ccp_res;
}

static void ccp_push_used_insns (MIR_context_t ctx, var_occ_t def) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  for (var_occ_t var_occ = DLIST_HEAD (var_occ_t, def->uses); var_occ != NULL;
       var_occ = DLIST_NEXT (var_occ_t, var_occ))
    if (var_occ->place.type == OCC_INSN) {
      bb_insn_t bb_insn = var_occ->place.u.insn->data;

      if (bb_insn->flag) continue; /* already in ccp_insns */
      VARR_PUSH (bb_insn_t, ccp_insns, bb_insn);
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "           pushing bb%lu insn: ", (unsigned long) bb_insn->bb->index);
        MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, TRUE);
      }
#endif
      bb_insn->flag = TRUE;
    } else {
      struct var_occ vos;
      var_occ_t tab_var_occ;

      gen_assert (var_occ->place.type == OCC_BB_END);
      for (edge_t e = DLIST_HEAD (out_edge_t, var_occ->place.u.bb->out_edges); e != NULL;
           e = DLIST_NEXT (out_edge_t, e)) {
        if (e->skipped_p) continue;
        vos = *var_occ;
        vos.place.type = OCC_BB_START;
        vos.place.u.bb = e->dst;
        if (!HTAB_DO (var_occ_t, var_occ_tab, &vos, HTAB_FIND, tab_var_occ) || tab_var_occ->flag)
          continue; /* var_occ at the start of BB in subsequent BB is already in ccp_var_occs */
#if !MIR_NO_GEN_DEBUG
        if (debug_file != NULL)
          fprintf (debug_file, "           pushing var%lu(%s) at start of bb%lu\n",
                   (long unsigned) vos.var,
                   var_is_reg_p (vos.var)
                     ? MIR_reg_name (ctx, var2reg (gen_ctx, vos.var), curr_func_item->u.func)
                     : "",
                   (unsigned long) e->dst->index);
#endif
        VARR_PUSH (var_occ_t, ccp_var_occs, tab_var_occ);
        tab_var_occ->flag = TRUE;
      }
    }
}

static void ccp_process_bb_start_var_occ (MIR_context_t ctx, var_occ_t var_occ, bb_t bb,
                                          int from_bb_process_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  struct var_occ vos;
  var_occ_t tab_var_occ, def;
  int change_p;

#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL)
    fprintf (debug_file,
             "       %sprocessing var%lu(%s) at start of bb%lu:", from_bb_process_p ? "  " : "",
             (long unsigned) var_occ->var,
             var_is_reg_p (var_occ->var)
               ? MIR_reg_name (ctx, var2reg (gen_ctx, var_occ->var), curr_func_item->u.func)
               : "",
             (unsigned long) var_occ->place.u.bb->index);
#endif
  gen_assert (var_occ->place.type == OCC_BB_START && bb == var_occ->place.u.bb);
  if (var_occ->val_kind == CCP_VARYING) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) fprintf (debug_file, " already varying\n");
#endif
    return;
  } else if (bb->index == 0) { /* Non-parameter at entry BB (it means using undefined value) */
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) fprintf (debug_file, " making varying\n");
#endif
    var_occ->val_kind = CCP_VARYING;
  }
  change_p = FALSE;
  for (edge_t e = DLIST_HEAD (in_edge_t, bb->in_edges); e != NULL; e = DLIST_NEXT (in_edge_t, e)) {
    /* Update var_occ value: */
    if (e->skipped_p) continue;
    vos.place.type = OCC_BB_END;
    vos.place.u.bb = e->src;
    vos.var = var_occ->var;
    if (!HTAB_DO (var_occ_t, var_occ_tab, &vos, HTAB_FIND, tab_var_occ)) {
      gen_assert (FALSE);
      return;
    }
    def = tab_var_occ->def;
    gen_assert (def != NULL);
    if (def->val_kind == CCP_UNKNOWN) continue;
    gen_assert (def->def == NULL && var_occ->def == NULL);
    if (var_occ->val_kind == CCP_UNKNOWN || def->val_kind == CCP_VARYING) {
      change_p = var_occ->val_kind != def->val_kind;
      var_occ->val_kind = def->val_kind;
      if (def->val_kind == CCP_VARYING) break;
      if (def->val_kind == CCP_CONST) var_occ->val = def->val;
    } else {
      gen_assert (var_occ->val_kind == CCP_CONST && def->val_kind == CCP_CONST);
      if (var_occ->val.uns_p != def->val.uns_p
          || (var_occ->val.uns_p && var_occ->val.u.u != def->val.u.u)
          || (!var_occ->val.uns_p && var_occ->val.u.i != def->val.u.i)) {
        var_occ->val_kind = CCP_VARYING;
        change_p = TRUE;
        break;
      }
    }
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    if (var_occ->val_kind != CCP_CONST) {
      fprintf (debug_file, "         %s%s\n", change_p ? "changed to " : "",
               var_occ->val_kind == CCP_UNKNOWN ? "unknown" : "varying");
    } else {
      fprintf (debug_file, "         %sconst ", change_p ? "changed to " : "");
      print_const (debug_file, var_occ->val);
      fprintf (debug_file, "\n");
    }
  }
#endif
  if (change_p) ccp_push_used_insns (ctx, var_occ);
}

static void ccp_process_active_edge (MIR_context_t ctx, edge_t e) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (e->skipped_p && !e->dst->flag) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL)
      fprintf (debug_file, "         Make edge bb%lu->bb%lu active\n",
               (unsigned long) e->src->index, (unsigned long) e->dst->index);
#endif
    e->dst->flag = TRUE; /* just activated edge whose dest is not in ccp_bbs */
    VARR_PUSH (bb_t, ccp_bbs, e->dst);
  }
  e->skipped_p = FALSE;
}

static void ccp_make_insn_update (MIR_context_t ctx, MIR_insn_t insn) {
  struct gen_ctx *gen_ctx MIR_UNUSED = *gen_ctx_loc (ctx);
  int i, def_p MIR_UNUSED;
  MIR_op_t op;
  var_occ_t var_occ;

  if (!ccp_insn_update (ctx, insn, NULL)) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      if (MIR_call_code_p (insn->code)) {
        fprintf (debug_file, " -- keep all results varying");
      } else if (get_ccp_res_op (ctx, insn, 0, &op) && var_insn_op_p (insn, 0)) {
        var_occ = op.data;
        if (var_occ->val_kind == CCP_UNKNOWN) {
          fprintf (debug_file, " -- make the result unknown");
        } else if (var_occ->val_kind == CCP_VARYING) {
          fprintf (debug_file, " -- keep the result varying");
        } else {
          gen_assert (var_occ->val_kind == CCP_CONST);
          fprintf (debug_file, " -- keep the result a constant ");
          print_const (debug_file, var_occ->val);
        }
      }
      fprintf (debug_file, "\n");
    }
#endif
  } else {
    def_p = FALSE;
    var_occ = NULL; /* to remove an initilized warning */
    for (i = 0; get_ccp_res_op (ctx, insn, i, &op); i++)
      if (var_op_p (op)) {
        def_p = TRUE;
        var_occ = op.data;
        ccp_push_used_insns (ctx, var_occ);
      }
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL && def_p) {
      if (MIR_call_code_p (insn->code)) {
        fprintf (debug_file, " -- make all results varying");
      } else if (var_occ->val_kind == CCP_VARYING) {
        fprintf (debug_file, " -- make the result varying\n");
      } else {
        gen_assert (var_occ->val_kind == CCP_CONST);
        fprintf (debug_file, " -- make the result a constant ");
        print_const (debug_file, var_occ->val);
        fprintf (debug_file, "\n");
      }
    }
#endif
  }
}

static void ccp_process_insn (MIR_context_t ctx, bb_insn_t bb_insn) {
  struct gen_ctx *gen_ctx MIR_UNUSED = *gen_ctx_loc (ctx);
  int res;
  enum ccp_val_kind ccp_res;
  edge_t e;
  bb_t bb = bb_insn->bb;
  MIR_insn_t insn = bb_insn->insn;

#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    fprintf (debug_file, "       processing bb%lu insn: ", (unsigned long) bb_insn->bb->index);
    MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, FALSE);
  }
#endif
  if (!MIR_branch_code_p (insn->code) || insn->code == MIR_JMP || insn->code == MIR_SWITCH) {
    ccp_make_insn_update (ctx, insn);
    return;
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) fprintf (debug_file, "\n");
#endif
  if ((ccp_res = ccp_branch_update (insn, &res)) == CCP_CONST) {
    /* Remember about an edge to exit bb.  First edge is always for
       fall through and the 2nd edge is for jump bb. */
    gen_assert (DLIST_LENGTH (out_edge_t, bb->out_edges) >= 2);
    e = res ? DLIST_EL (out_edge_t, bb->out_edges, 1) : DLIST_EL (out_edge_t, bb->out_edges, 0);
    ccp_process_active_edge (ctx, e);
  } else if (ccp_res == CCP_VARYING) { /* activate all edges */
    for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_NEXT (out_edge_t, e))
      ccp_process_active_edge (ctx, e);
  }
}

static void ccp_process_bb (MIR_context_t ctx, bb_t bb) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_insn_t bb_insn;
  edge_t e;

#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL)
    fprintf (debug_file, "       processing bb%lu\n", (unsigned long) bb->index);
#endif
  for (var_occ_t var_occ = DLIST_HEAD (var_occ_t, bb_start_occ_lists[bb->index]); var_occ != NULL;
       var_occ = DLIST_NEXT (var_occ_t, var_occ))
    ccp_process_bb_start_var_occ (ctx, var_occ, bb, TRUE);
  if (!bitmap_set_bit_p (bb_visited, bb->index)) return;
  for (bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
       bb_insn = DLIST_NEXT (bb_insn_t, bb_insn)) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "         processing insn: ");
      MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, FALSE);
    }
#endif
    ccp_make_insn_update (ctx, bb_insn->insn);
  }
  if ((bb_insn = DLIST_TAIL (bb_insn_t, bb->bb_insns)) == NULL
      || !MIR_branch_code_p (bb_insn->insn->code) || bb_insn->insn->code == MIR_JMP
      || bb_insn->insn->code == MIR_SWITCH) {
    for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_NEXT (out_edge_t, e)) {
      gen_assert (!e->skipped_p);
      if (!bitmap_bit_p (bb_visited, e->dst->index) && !e->dst->flag) {
        e->dst->flag = TRUE; /* first process of dest which is not in ccp_bbs */
        VARR_PUSH (bb_t, ccp_bbs, e->dst);
      }
      ccp_process_active_edge (ctx, e);
    }
  }
}

static void ccp_traverse (bb_t bb) {
  bb->flag = TRUE;
  for (edge_t e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_NEXT (out_edge_t, e))
    if (!e->skipped_p && !e->dst->flag)
      ccp_traverse (e->dst); /* visit unvisited active edge destination */
}

static int get_ccp_res_val (MIR_context_t ctx, MIR_insn_t insn, const_t *val) {
  var_occ_t var_occ;
  MIR_op_t op;

  if (MIR_call_code_p (insn->code) || !get_ccp_res_op (ctx, insn, 0, &op))
    return FALSE; /* call results always produce varying values */
  if (!var_insn_op_p (insn, 0)) return FALSE;
  var_occ = op.data;
  gen_assert (var_occ->def == NULL);
  if (var_occ->val_kind != CCP_CONST) return FALSE;
  *val = var_occ->val;
  return TRUE;
}

static int ccp_modify (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  bb_t bb, next_bb;
  bb_insn_t bb_insn, next_bb_insn;
  const_t val;
  MIR_op_t op;
  MIR_insn_t insn, prev_insn, first_insn;
  int res, change_p = FALSE;

#ifndef NDEBUG
  for (bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb))
    gen_assert (!bb->flag);
#endif
  ccp_traverse (DLIST_HEAD (bb_t, curr_cfg->bbs)); /* entry */
  for (bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = next_bb) {
    next_bb = DLIST_NEXT (bb_t, bb);
    if (!bb->flag) {
      change_p = TRUE;
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL)
        fprintf (debug_file, "  deleting unreachable bb%lu and its edges\n",
                 (unsigned long) bb->index);
#endif
      for (bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL;
           bb_insn = next_bb_insn) {
        next_bb_insn = DLIST_NEXT (bb_insn_t, bb_insn);
        insn = bb_insn->insn;
        gen_delete_insn (ctx, insn);
      }
      delete_bb (ctx, bb);
      continue;
    }
    bb->flag = FALSE; /* reset for the future use */
    for (bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL; bb_insn = next_bb_insn) {
      next_bb_insn = DLIST_NEXT (bb_insn_t, bb_insn);
      if (get_ccp_res_val (ctx, bb_insn->insn, &val)
          && (bb_insn->insn->code != MIR_MOV
              || (bb_insn->insn->ops[1].mode != MIR_OP_INT
                  && bb_insn->insn->ops[1].mode != MIR_OP_UINT))) {
        gen_assert (!MIR_call_code_p (bb_insn->insn->code));
        change_p = TRUE;
#if !MIR_NO_GEN_DEBUG
        if (debug_file != NULL) {
          fprintf (debug_file, "  changing insn ");
          MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, FALSE);
        }
#endif
        op = val.uns_p ? MIR_new_uint_op (ctx, val.u.u) : MIR_new_int_op (ctx, val.u.i);
#ifndef NDEBUG
        {
          int out_p;

          MIR_insn_op_mode (ctx, bb_insn->insn, 0, &out_p); /* result here is always 0-th op */
          gen_assert (out_p);
        }
#endif
        insn = MIR_new_insn (ctx, MIR_MOV, bb_insn->insn->ops[0], op);
        MIR_insert_insn_before (ctx, curr_func_item, bb_insn->insn, insn);
        MIR_remove_insn (ctx, curr_func_item, bb_insn->insn);
        insn->data = bb_insn;
        bb_insn->insn = insn;
#if !MIR_NO_GEN_DEBUG
        if (debug_file != NULL) {
          fprintf (debug_file, "    on insn ");
          MIR_output_insn (ctx, debug_file, insn, curr_func_item->u.func, TRUE);
        }
#endif
      }
      // nulify/free op.data ???
    }
    if ((bb_insn = DLIST_TAIL (bb_insn_t, bb->bb_insns)) == NULL) continue;
    insn = bb_insn->insn;
    first_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns)->insn;
    if (first_insn->code == MIR_LABEL && (prev_insn = DLIST_PREV (MIR_insn_t, first_insn)) != NULL
        && prev_insn->code == MIR_JMP && prev_insn->ops[0].u.label == first_insn) {
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "  removing useless jump insn ");
        MIR_output_insn (ctx, debug_file, prev_insn, curr_func_item->u.func, TRUE);
        fprintf (debug_file, "\n");
      }
#endif
      gen_delete_insn (ctx, prev_insn);
    }
    if (!MIR_branch_code_p (insn->code) || insn->code == MIR_JMP || insn->code == MIR_SWITCH
        || ccp_branch_update (insn, &res) != CCP_CONST)
      continue;
    change_p = TRUE;
    if (!res) {
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "  removing branch insn ");
        MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, TRUE);
        fprintf (debug_file, "\n");
      }
#endif
      gen_delete_insn (ctx, insn);
      delete_edge (DLIST_EL (out_edge_t, bb->out_edges, 1));
    } else {
      insn = MIR_new_insn (ctx, MIR_JMP, bb_insn->insn->ops[0]); /* label is always 0-th op */
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "  changing branch insn ");
        MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, FALSE);
        fprintf (debug_file, " onto jump insn ");
        MIR_output_insn (ctx, debug_file, insn, curr_func_item->u.func, TRUE);
        fprintf (debug_file, "\n");
      }
#endif
      MIR_insert_insn_before (ctx, curr_func_item, bb_insn->insn, insn);
      MIR_remove_insn (ctx, curr_func_item, bb_insn->insn);
      insn->data = bb_insn;
      bb_insn->insn = insn;
      delete_edge (DLIST_EL (out_edge_t, bb->out_edges, 0));
    }
  }
  return change_p;
}

static int ccp (MIR_context_t ctx) { /* conditional constant propagation */
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) fprintf (debug_file, "  CCP analysis:\n");
#endif
  build_var_occ_web (ctx);
  bb_visited = temp_bitmap;
  initiate_ccp_info (ctx);
  while (VARR_LENGTH (bb_t, ccp_bbs) != 0 || VARR_LENGTH (var_occ_t, ccp_var_occs) != 0
         || VARR_LENGTH (bb_insn_t, ccp_insns) != 0) {
    while (VARR_LENGTH (bb_t, ccp_bbs) != 0) {
      bb_t bb = VARR_POP (bb_t, ccp_bbs);

      bb->flag = FALSE;
      ccp_process_bb (ctx, bb);
    }
    while (VARR_LENGTH (var_occ_t, ccp_var_occs) != 0) {
      var_occ_t var_occ = VARR_POP (var_occ_t, ccp_var_occs);

      var_occ->flag = FALSE;
      gen_assert (var_occ->place.type == OCC_BB_START);
      ccp_process_bb_start_var_occ (ctx, var_occ, var_occ->place.u.bb, FALSE);
    }
    while (VARR_LENGTH (bb_insn_t, ccp_insns) != 0) {
      bb_insn_t bb_insn = VARR_POP (bb_insn_t, ccp_insns);

      gen_assert (bb_insn->flag);
      bb_insn->flag = FALSE;
      ccp_process_insn (ctx, bb_insn);
    }
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) fprintf (debug_file, "  CCP modification:\n");
#endif
  return ccp_modify (ctx);
}

static void ccp_clear (MIR_context_t ctx) { var_occs_clear (ctx); }

static void init_ccp (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->ccp_ctx = gen_malloc (ctx, sizeof (struct ccp_ctx));
  init_var_occs (ctx);
  VARR_CREATE (bb_t, ccp_bbs, 256);
  VARR_CREATE (var_occ_t, ccp_var_occs, 256);
  VARR_CREATE (bb_insn_t, ccp_insns, 256);
}

static void finish_ccp (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  finish_var_occs (ctx);
  VARR_DESTROY (bb_t, ccp_bbs);
  VARR_DESTROY (var_occ_t, ccp_var_occs);
  VARR_DESTROY (bb_insn_t, ccp_insns);
  free (gen_ctx->ccp_ctx);
  gen_ctx->ccp_ctx = NULL;
}

#undef live_in
#undef live_out

/* New Page */

#define live_in in
#define live_out out
#define live_kill kill
#define live_gen gen

/* Life analysis */
static void live_con_func_0 (bb_t bb) { bitmap_clear (bb->live_in); }

static int live_con_func_n (MIR_context_t ctx, bb_t bb) {
  edge_t e;
  int change_p = FALSE;

  for (e = DLIST_HEAD (out_edge_t, bb->out_edges); e != NULL; e = DLIST_NEXT (out_edge_t, e))
    change_p |= bitmap_ior (bb->live_out, bb->live_out, e->dst->live_in);
  return change_p;
}

static int live_trans_func (MIR_context_t ctx, bb_t bb) {
  return bitmap_ior_and_compl (bb->live_in, bb->live_gen, bb->live_out, bb->live_kill);
}

static int bb_loop_level (bb_t bb) {
  loop_node_t loop_node;
  int level = -1;

  for (loop_node = bb->loop_node; loop_node->parent != NULL; loop_node = loop_node->parent) level++;
  gen_assert (level >= 0);
  return level;
}

static void increase_pressure (int int_p, bb_t bb, int *int_pressure, int *fp_pressure) {
  if (int_p) {
    if (bb->max_int_pressure < ++(*int_pressure)) bb->max_int_pressure = *int_pressure;
  } else {
    if (bb->max_fp_pressure < ++(*fp_pressure)) bb->max_fp_pressure = *fp_pressure;
  }
}

static size_t initiate_bb_live_info (MIR_context_t ctx, bb_t bb, int moves_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn;
  size_t i, niter, passed_mem_num, bb_freq, mvs_num = 0;
  MIR_reg_t var, early_clobbered_hard_reg1, early_clobbered_hard_reg2;
  int out_p, mem_p, int_p;
  int bb_int_pressure, max_bb_int_pressure, bb_fp_pressure, max_bb_fp_pressure;
  mv_t mv;
  reg_info_t *breg_infos;
  insn_var_iterator_t insn_var_iter;

  gen_assert (bb->live_in != NULL && bb->live_out != NULL && bb->live_gen != NULL
              && bb->live_kill != NULL);
  bitmap_clear (bb->live_in);
  bitmap_clear (bb->live_out);
  bitmap_clear (bb->live_gen);
  bitmap_clear (bb->live_kill);
  breg_infos = VARR_ADDR (reg_info_t, curr_cfg->breg_info);
  bb_freq = 1;
  if (moves_p)
    for (int i = bb_loop_level (bb); i > 0; i--) bb_freq *= 5;
  bb->max_int_pressure = bb_int_pressure = bb->max_fp_pressure = bb_fp_pressure = 0;
  for (bb_insn_t bb_insn = DLIST_TAIL (bb_insn_t, bb->bb_insns); bb_insn != NULL;
       bb_insn = DLIST_PREV (bb_insn_t, bb_insn)) {
    insn = bb_insn->insn;
    if (MIR_call_code_p (insn->code)) {
      bitmap_ior (bb->live_kill, bb->live_kill, call_used_hard_regs);
      bitmap_and_compl (bb->live_gen, bb->live_gen, call_used_hard_regs);
    }
    /* Process output ops on 0-th iteration, then input ops. */
    for (niter = 0; niter <= 1; niter++) {
      FOREACH_INSN_VAR (ctx, insn_var_iter, insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p && niter != 0) {
          if (bitmap_set_bit_p (bb->live_gen, var))
            increase_pressure (int_var_type_p (ctx, var), bb, &bb_int_pressure, &bb_fp_pressure);
        } else if (niter == 0) {
          if (bitmap_clear_bit_p (bb->live_gen, var))
            (int_var_type_p (ctx, var) ? bb_int_pressure-- : bb_fp_pressure--);
          bitmap_set_bit_p (bb->live_kill, var);
        }
        if (var_is_reg_p (var)) breg_infos[var2breg (gen_ctx, var)].freq += bb_freq;
      }
    }
    target_get_early_clobbered_hard_regs (insn, &early_clobbered_hard_reg1,
                                          &early_clobbered_hard_reg2);
    if (early_clobbered_hard_reg1 != MIR_NON_HARD_REG) {
      int_p = int_var_type_p (ctx, early_clobbered_hard_reg1);
      increase_pressure (int_p, bb, &bb_int_pressure, &bb_fp_pressure);
      bitmap_clear_bit_p (bb->live_gen, early_clobbered_hard_reg1);
      bitmap_set_bit_p (bb->live_kill, early_clobbered_hard_reg1);
      (int_p ? bb_int_pressure-- : bb_fp_pressure--);
    }
    if (early_clobbered_hard_reg2 != MIR_NON_HARD_REG) {
      int_p = int_var_type_p (ctx, early_clobbered_hard_reg2);
      increase_pressure (int_p, bb, &bb_int_pressure, &bb_fp_pressure);
      bitmap_clear_bit_p (bb->live_gen, early_clobbered_hard_reg2);
      bitmap_set_bit_p (bb->live_kill, early_clobbered_hard_reg2);
      (int_p ? bb_int_pressure-- : bb_fp_pressure--);
    }
    if (MIR_call_code_p (insn->code))
      bitmap_ior (bb->live_gen, bb->live_gen, bb_insn->call_hard_reg_args);
    if (moves_p && move_p (insn)) {
      mv = get_free_move (ctx);
      mv->bb_insn = bb_insn;
      mv->freq = bb_freq;
      if (insn->ops[0].mode == MIR_OP_REG)
        DLIST_APPEND (dst_mv_t, breg_infos[reg2breg (gen_ctx, insn->ops[0].u.reg)].dst_moves, mv);
      if (insn->ops[1].mode == MIR_OP_REG)
        DLIST_APPEND (src_mv_t, breg_infos[reg2breg (gen_ctx, insn->ops[1].u.reg)].src_moves, mv);
      mvs_num++;
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "  move with freq %10lu:", (unsigned long) mv->freq);
        MIR_output_insn (ctx, debug_file, bb_insn->insn, curr_func_item->u.func, TRUE);
      }
#endif
    }
  }
  return mvs_num;
}

static void initiate_live_info (MIR_context_t ctx, int moves_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t nregs, n;
  mv_t mv, next_mv;
  reg_info_t ri;
  size_t mvs_num = 0;

  for (mv = DLIST_HEAD (mv_t, curr_cfg->used_moves); mv != NULL; mv = next_mv) {
    next_mv = DLIST_NEXT (mv_t, mv);
    free_move (ctx, mv);
  }
  VARR_TRUNC (reg_info_t, curr_cfg->breg_info, 0);
  nregs = get_nregs (ctx);
  for (n = 0; n < nregs; n++) {
    ri.freq = ri.thread_freq = ri.calls_num = 0;
    ri.live_length = 0;
    ri.thread_first = n;
    ri.thread_next = MIR_MAX_REG_NUM;
    DLIST_INIT (dst_mv_t, ri.dst_moves);
    DLIST_INIT (src_mv_t, ri.src_moves);
    VARR_PUSH (reg_info_t, curr_cfg->breg_info, ri);
  }
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb))
    mvs_num += initiate_bb_live_info (ctx, bb, moves_p);
  if (moves_p) curr_cfg->non_conflicting_moves = mvs_num;
}

static void update_bb_pressure (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t nel;
  bitmap_iterator_t bi;

  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    int int_pressure = bb->max_int_pressure, fp_pressure = bb->max_fp_pressure;

    FOREACH_BITMAP_BIT (bi, bb->live_out, nel) {
      increase_pressure (int_var_type_p (ctx, (MIR_reg_t) nel), bb, &int_pressure, &fp_pressure);
    }
  }
}

static void calculate_func_cfg_live_info (MIR_context_t ctx, int moves_p) {
  initiate_live_info (ctx, moves_p);
  solve_dataflow (ctx, FALSE, live_con_func_0, live_con_func_n, live_trans_func);
  update_bb_pressure (ctx);
}

static void add_bb_insn_dead_vars (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn;
  bb_insn_t bb_insn, prev_bb_insn;
  size_t passed_mem_num;
  MIR_reg_t var, early_clobbered_hard_reg1, early_clobbered_hard_reg2;
  MIR_op_t op;
  int out_p, mem_p;
  bitmap_t live;
  insn_var_iterator_t insn_var_iter;

  live = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    bitmap_copy (live, bb->live_out);
    for (bb_insn = DLIST_TAIL (bb_insn_t, bb->bb_insns); bb_insn != NULL; bb_insn = prev_bb_insn) {
      prev_bb_insn = DLIST_PREV (bb_insn_t, bb_insn);
      clear_bb_insn_dead_vars (bb_insn);
      insn = bb_insn->insn;
      FOREACH_INSN_VAR (ctx, insn_var_iter, insn, var, out_p, mem_p, passed_mem_num) {
        if (out_p) bitmap_clear_bit_p (live, var);
      }
      if (MIR_call_code_p (insn->code)) bitmap_and_compl (live, live, call_used_hard_regs);
      FOREACH_INSN_VAR (ctx, insn_var_iter, insn, var, out_p, mem_p, passed_mem_num) {
        if (out_p) continue;
        if (bitmap_set_bit_p (live, var)) add_bb_insn_dead_var (ctx, bb_insn, var);
      }
      target_get_early_clobbered_hard_regs (insn, &early_clobbered_hard_reg1,
                                            &early_clobbered_hard_reg2);
      if (early_clobbered_hard_reg1 != MIR_NON_HARD_REG)
        bitmap_clear_bit_p (live, early_clobbered_hard_reg1);
      if (early_clobbered_hard_reg2 != MIR_NON_HARD_REG)
        bitmap_clear_bit_p (live, early_clobbered_hard_reg2);
      if (MIR_call_code_p (insn->code)) bitmap_ior (live, live, bb_insn->call_hard_reg_args);
    }
  }
  bitmap_destroy (live);
}

typedef struct live_range *live_range_t; /* vars */

struct live_range {
  int start, finish;
  live_range_t next;
};

DEF_VARR (live_range_t);

struct lr_ctx {
  int curr_point;
  bitmap_t live_vars;
  VARR (live_range_t) * var_live_ranges;
};

#define curr_point gen_ctx->lr_ctx->curr_point
#define live_vars gen_ctx->lr_ctx->live_vars
#define var_live_ranges gen_ctx->lr_ctx->var_live_ranges

static live_range_t create_live_range (MIR_context_t ctx, int start, int finish,
                                       live_range_t next) {
  live_range_t lr = gen_malloc (ctx, sizeof (struct live_range));

  gen_assert (finish < 0 || start <= finish);
  lr->start = start;
  lr->finish = finish;
  lr->next = next;
  return lr;
}

static void destroy_live_range (live_range_t lr) {
  live_range_t next_lr;

  for (; lr != NULL; lr = next_lr) {
    next_lr = lr->next;
    free (lr);
  }
}

static int make_var_dead (MIR_context_t ctx, MIR_reg_t var, int point) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  live_range_t lr;

  if (bitmap_clear_bit_p (live_vars, var)) {
    lr = VARR_GET (live_range_t, var_live_ranges, var);
    lr->finish = point;
  } else { /* insn with unused result: result still needs a register */
    VARR_SET (live_range_t, var_live_ranges, var,
              create_live_range (ctx, point, point, VARR_GET (live_range_t, var_live_ranges, var)));
  }
  return TRUE;
}

static int make_var_live (MIR_context_t ctx, MIR_reg_t var, int point) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  live_range_t lr;

  if (!bitmap_set_bit_p (live_vars, var)) return FALSE;
  if ((lr = VARR_GET (live_range_t, var_live_ranges, var)) == NULL
      || (lr->finish != point && lr->finish + 1 != point))
    VARR_SET (live_range_t, var_live_ranges, var, create_live_range (ctx, point, -1, lr));
  return TRUE;
}

#if !MIR_NO_GEN_DEBUG
static void print_live_ranges (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t i;
  live_range_t lr;

  fprintf (debug_file, "+++++++++++++Live ranges:\n");
  gen_assert (get_nvars (ctx) == VARR_LENGTH (live_range_t, var_live_ranges));
  for (i = 0; i < VARR_LENGTH (live_range_t, var_live_ranges); i++) {
    if ((lr = VARR_GET (live_range_t, var_live_ranges, i)) == NULL) continue;
    fprintf (debug_file, "%lu", i);
    if (var_is_reg_p (i))
      fprintf (debug_file, " (%s:%s)",
               MIR_type_str (ctx, MIR_reg_type (ctx, var2reg (gen_ctx, i), curr_func_item->u.func)),
               MIR_reg_name (ctx, var2reg (gen_ctx, i), curr_func_item->u.func));
    fprintf (debug_file, ":");
    for (; lr != NULL; lr = lr->next) fprintf (debug_file, " [%d..%d]", lr->start, lr->finish);
    fprintf (debug_file, "\n");
  }
}
#endif

static void build_live_ranges (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn;
  MIR_reg_t var, nvars, early_clobbered_hard_reg1, early_clobbered_hard_reg2;
  size_t i, nel, passed_mem_num;
  int incr_p, out_p, mem_p;
  bitmap_iterator_t bi;
  insn_var_iterator_t insn_var_iter;

  curr_point = 0;
  nvars = get_nvars (ctx);
  gen_assert (VARR_LENGTH (live_range_t, var_live_ranges) == 0);
  for (i = 0; i < nvars; i++) VARR_PUSH (live_range_t, var_live_ranges, NULL);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL)
      fprintf (debug_file, "  ------BB%u end: point=%d\n", (unsigned) bb->index, curr_point);
#endif
    bitmap_clear (live_vars);
    if (bb->live_out != NULL) FOREACH_BITMAP_BIT (bi, bb->live_out, nel) {
        make_var_live (ctx, nel, curr_point);
      }
    for (bb_insn_t bb_insn = DLIST_TAIL (bb_insn_t, bb->bb_insns); bb_insn != NULL;
         bb_insn = DLIST_PREV (bb_insn_t, bb_insn)) {
      insn = bb_insn->insn;
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "  p%-5d", curr_point);
        print_bb_insn (ctx, bb_insn, TRUE);
      }
#endif
      incr_p = FALSE;
      FOREACH_INSN_VAR (ctx, insn_var_iter, insn, var, out_p, mem_p, passed_mem_num) {
        if (out_p) incr_p |= make_var_dead (ctx, var, curr_point);
      }
      if (MIR_call_code_p (insn->code)) {
        FOREACH_BITMAP_BIT (bi, call_used_hard_regs, nel) { make_var_dead (ctx, nel, curr_point); }
        FOREACH_BITMAP_BIT (bi, bb_insn->call_hard_reg_args, nel) {
          make_var_live (ctx, nel, curr_point);
        }
        FOREACH_BITMAP_BIT (bi, live_vars, nel) {
          reg_info_t *bri;
          MIR_reg_t breg;

          if (!var_is_reg_p (nel)) continue;
          breg = reg2breg (gen_ctx, var2reg (gen_ctx, nel));
          bri = &VARR_ADDR (reg_info_t, curr_cfg->breg_info)[breg];
          bri->calls_num++;
        }
      }
      if (incr_p) curr_point++;
      incr_p = FALSE;
      FOREACH_INSN_VAR (ctx, insn_var_iter, insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p) incr_p |= make_var_live (ctx, var, curr_point);
      }
      target_get_early_clobbered_hard_regs (insn, &early_clobbered_hard_reg1,
                                            &early_clobbered_hard_reg2);
      if (early_clobbered_hard_reg1 != MIR_NON_HARD_REG) {
        incr_p |= make_var_live (ctx, early_clobbered_hard_reg1, curr_point);
        incr_p |= make_var_dead (ctx, early_clobbered_hard_reg1, curr_point);
      }
      if (early_clobbered_hard_reg2 != MIR_NON_HARD_REG) {
        incr_p |= make_var_live (ctx, early_clobbered_hard_reg2, curr_point);
        incr_p |= make_var_dead (ctx, early_clobbered_hard_reg2, curr_point);
      }
      if (incr_p) curr_point++;
    }
    gen_assert (bitmap_equal_p (live_vars, bb->live_in));
    FOREACH_BITMAP_BIT (bi, live_vars, nel) { make_var_dead (ctx, nel, curr_point); }
    if (!bitmap_empty_p (bb->live_in)) curr_point++;
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) print_live_ranges (ctx);
#endif
}

static void destroy_func_live_ranges (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t i;

  for (i = 0; i < VARR_LENGTH (live_range_t, var_live_ranges); i++)
    destroy_live_range (VARR_GET (live_range_t, var_live_ranges, i));
  VARR_TRUNC (live_range_t, var_live_ranges, 0);
}

#if !MIR_NO_GEN_DEBUG
static void output_bb_live_info (MIR_context_t ctx, bb_t bb) {
  output_bitmap (ctx, "  live_in:", bb->live_in, TRUE);
  output_bitmap (ctx, "  live_out:", bb->live_out, TRUE);
  output_bitmap (ctx, "  live_gen:", bb->live_gen, TRUE);
  output_bitmap (ctx, "  live_kill:", bb->live_kill, TRUE);
}
#endif

static void init_live_ranges (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->lr_ctx = gen_malloc (ctx, sizeof (struct lr_ctx));
  VARR_CREATE (live_range_t, var_live_ranges, 0);
  live_vars = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
}

static void finish_live_ranges (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (live_range_t, var_live_ranges);
  bitmap_destroy (live_vars);
  free (gen_ctx->lr_ctx);
  gen_ctx->lr_ctx = NULL;
}

#undef live_in
#undef live_out
#undef live_kill
#undef live_gen

/* New Page */

/* Register allocation */

DEF_VARR (MIR_reg_t);

typedef struct breg_info {
  MIR_reg_t breg;
  reg_info_t *breg_infos;
} breg_info_t;

DEF_VARR (breg_info_t);

struct ra_ctx {
  VARR (MIR_reg_t) * breg_renumber;
  VARR (breg_info_t) * sorted_bregs;
  VARR (bitmap_t) * point_used_locs;
  bitmap_t conflict_locs;
  reg_info_t *curr_breg_infos;
  VARR (size_t) * loc_profits;
  VARR (size_t) * loc_profit_ages;
  size_t curr_age;
  /* Slots num for variables.  Some variable can take several slots. */
  size_t func_stack_slots_num;
  bitmap_t func_assigned_hard_regs;
};

#define breg_renumber gen_ctx->ra_ctx->breg_renumber
#define sorted_bregs gen_ctx->ra_ctx->sorted_bregs
#define point_used_locs gen_ctx->ra_ctx->point_used_locs
#define conflict_locs gen_ctx->ra_ctx->conflict_locs
#define curr_breg_infos gen_ctx->ra_ctx->curr_breg_infos
#define loc_profits gen_ctx->ra_ctx->loc_profits
#define loc_profit_ages gen_ctx->ra_ctx->loc_profit_ages
#define curr_age gen_ctx->ra_ctx->curr_age
#define func_stack_slots_num gen_ctx->ra_ctx->func_stack_slots_num
#define func_assigned_hard_regs gen_ctx->ra_ctx->func_assigned_hard_regs

static void process_move_to_form_thread (MIR_context_t ctx, mv_t mv) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_op_t op1 = mv->bb_insn->insn->ops[0], op2 = mv->bb_insn->insn->ops[1];
  MIR_reg_t breg1, breg2, breg1_first, breg2_first, last;

  if (op1.mode != MIR_OP_REG || op2.mode != MIR_OP_REG) return;
  breg1 = reg2breg (gen_ctx, op1.u.reg);
  breg2 = reg2breg (gen_ctx, op2.u.reg);
  breg1_first = curr_breg_infos[breg1].thread_first;
  breg2_first = curr_breg_infos[breg2].thread_first;
  if (breg1_first != breg2_first) {
    for (last = breg2_first; curr_breg_infos[last].thread_next != MIR_MAX_REG_NUM;
         last = curr_breg_infos[last].thread_next)
      curr_breg_infos[last].thread_first = breg1_first;
    curr_breg_infos[last].thread_first = breg1_first;
    curr_breg_infos[last].thread_next = curr_breg_infos[breg1_first].thread_next;
    curr_breg_infos[breg1_first].thread_next = breg2_first;
    curr_breg_infos[breg1_first].thread_freq += curr_breg_infos[breg2_first].thread_freq;
  }
  curr_breg_infos[breg1_first].thread_freq -= 2 * mv->freq;
  gen_assert (curr_breg_infos[breg1_first].thread_freq >= 0);
}

static int breg_info_compare_func (const void *a1, const void *a2) {
  breg_info_t breg_info1 = *(const breg_info_t *) a1, breg_info2 = *(const breg_info_t *) a2;
  MIR_reg_t breg1 = breg_info1.breg, breg2 = breg_info2.breg;
  reg_info_t *breg_infos = breg_info1.breg_infos;
  MIR_reg_t t1 = breg_infos[breg1].thread_first, t2 = breg_infos[breg2].thread_first;
  long diff;

  gen_assert (breg_infos == breg_info2.breg_infos);
  if ((diff = breg_infos[t2].thread_freq - breg_infos[t1].thread_freq) != 0) return diff;
  if (t1 < t2) return -1;
  if (t2 < t1) return 1;
  if (breg_infos[breg2].live_length < breg_infos[breg1].live_length) return -1;
  if (breg_infos[breg1].live_length < breg_infos[breg2].live_length) return 1;
  return breg1 < breg2 ? -1 : 1; /* make sort stable */
}

static void setup_loc_profit_from_op (MIR_context_t ctx, MIR_op_t op, size_t freq) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t loc;
  size_t *curr_loc_profits = VARR_ADDR (size_t, loc_profits);
  size_t *curr_loc_profit_ages = VARR_ADDR (size_t, loc_profit_ages);

  if (op.mode == MIR_OP_HARD_REG)
    loc = op.u.hard_reg;
  else if ((loc = VARR_GET (MIR_reg_t, breg_renumber, reg2breg (gen_ctx, op.u.reg)))
           == MIR_NON_HARD_REG)
    return;
  if (curr_loc_profit_ages[loc] == curr_age)
    curr_loc_profits[loc] += freq;
  else {
    curr_loc_profit_ages[loc] = curr_age;
    curr_loc_profits[loc] = freq;
  }
}

static void setup_loc_profits (MIR_context_t ctx, MIR_reg_t breg) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  mv_t mv;
  reg_info_t *info = &curr_breg_infos[breg];

  for (mv = DLIST_HEAD (dst_mv_t, info->dst_moves); mv != NULL; mv = DLIST_NEXT (dst_mv_t, mv))
    setup_loc_profit_from_op (ctx, mv->bb_insn->insn->ops[1], mv->freq);
  for (mv = DLIST_HEAD (src_mv_t, info->src_moves); mv != NULL; mv = DLIST_NEXT (src_mv_t, mv))
    setup_loc_profit_from_op (ctx, mv->bb_insn->insn->ops[1], mv->freq);
}

static void assign (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t loc, best_loc, i, reg, breg, var, nregs = get_nregs (ctx);
  MIR_type_t type;
  int slots_num;
  int j, k;
  live_range_t lr;
  bitmap_t bm;
  size_t length, profit, best_profit;
  bitmap_t *point_used_locs_addr;
  breg_info_t breg_info;

  func_stack_slots_num = 0;
  if (nregs == 0) return;
  curr_breg_infos = VARR_ADDR (reg_info_t, curr_cfg->breg_info);
  VARR_TRUNC (MIR_reg_t, breg_renumber, 0);
  for (i = 0; i < nregs; i++) {
    VARR_PUSH (MIR_reg_t, breg_renumber, MIR_NON_HARD_REG);
    curr_breg_infos[i].thread_freq = curr_breg_infos[i].freq;
  }
  for (mv_t mv = DLIST_HEAD (mv_t, curr_cfg->used_moves); mv != NULL; mv = DLIST_NEXT (mv_t, mv))
    process_move_to_form_thread (ctx, mv);
  /* min_reg, max_reg for func */
  VARR_TRUNC (breg_info_t, sorted_bregs, 0);
  breg_info.breg_infos = curr_breg_infos;
  for (i = 0; i < nregs; i++) {
    breg_info.breg = i;
    VARR_PUSH (breg_info_t, sorted_bregs, breg_info);
    var = reg2var (gen_ctx, breg2reg (gen_ctx, i));
    for (length = 0, lr = VARR_GET (live_range_t, var_live_ranges, var); lr != NULL; lr = lr->next)
      length += lr->finish - lr->start + 1;
    curr_breg_infos[i].live_length = length;
  }
  VARR_TRUNC (size_t, loc_profits, 0);
  VARR_TRUNC (size_t, loc_profit_ages, 0);
  for (i = 0; i <= MAX_HARD_REG; i++) {
    VARR_PUSH (size_t, loc_profits, 0);
    VARR_PUSH (size_t, loc_profit_ages, 0);
  }
  VARR_TRUNC (bitmap_t, point_used_locs, 0);
  for (i = 0; i <= curr_point; i++) {
    bm = bitmap_create2 (MAX_HARD_REG + 1);
    VARR_PUSH (bitmap_t, point_used_locs, bm);
  }
  qsort (VARR_ADDR (breg_info_t, sorted_bregs), nregs, sizeof (breg_info_t),
         breg_info_compare_func);
  curr_age = 0;
  point_used_locs_addr = VARR_ADDR (bitmap_t, point_used_locs);
  for (i = 0; i <= MAX_HARD_REG; i++) {
    for (lr = VARR_GET (live_range_t, var_live_ranges, i); lr != NULL; lr = lr->next)
      for (j = lr->start; j <= lr->finish; j++) bitmap_set_bit_p (point_used_locs_addr[j], i);
  }
  bitmap_clear (func_assigned_hard_regs);
  for (i = 0; i < nregs; i++) { /* hard reg and stack slot assignment */
    breg = VARR_GET (breg_info_t, sorted_bregs, i).breg;
    if (VARR_GET (MIR_reg_t, breg_renumber, breg) != MIR_NON_HARD_REG) continue;
    reg = breg2reg (gen_ctx, breg);
    var = reg2var (gen_ctx, reg);
    bitmap_clear (conflict_locs);
    for (lr = VARR_GET (live_range_t, var_live_ranges, var); lr != NULL; lr = lr->next)
      for (j = lr->start; j <= lr->finish; j++)
        bitmap_ior (conflict_locs, conflict_locs, point_used_locs_addr[j]);
    curr_age++;
    setup_loc_profits (ctx, breg);
    best_loc = MIR_NON_HARD_REG;
    best_profit = 0;
    type = MIR_reg_type (ctx, reg, curr_func_item->u.func);
    for (loc = 0; loc <= func_stack_slots_num + MAX_HARD_REG; loc++) {
      if (loc <= MAX_HARD_REG && !target_hard_reg_type_ok_p (loc, type)) continue;
      slots_num = target_locs_num (loc, type);
      if (loc + slots_num - 1 > func_stack_slots_num + MAX_HARD_REG) break;
      for (k = 0; k < slots_num; k++)
        if ((loc + k <= MAX_HARD_REG
             && (target_fixed_hard_reg_p (loc + k)
                 || (target_call_used_hard_reg_p (loc + k) && curr_breg_infos[breg].calls_num > 0)))
            || bitmap_bit_p (conflict_locs, loc + k))
          break;
      if (k < slots_num) continue;
      if (loc > MAX_HARD_REG && (loc - MAX_HARD_REG - 1) % slots_num != 0)
        continue; /* we align stack slots according to the type size */
      profit = (VARR_GET (size_t, loc_profit_ages, loc) != curr_age
                  ? 0
                  : VARR_GET (size_t, loc_profits, loc));
      if (best_loc == MIR_NON_HARD_REG || best_profit < profit) {
        best_loc = loc;
        best_profit = profit;
      }
      if (best_loc != MIR_NON_HARD_REG && loc == MAX_HARD_REG) break;
    }
    slots_num = target_locs_num (best_loc, type);
    if (best_loc <= MAX_HARD_REG) {
      for (k = 0; k < slots_num; k++) bitmap_set_bit_p (func_assigned_hard_regs, best_loc + k);
    } else if (best_loc == MIR_NON_HARD_REG) { /* Add stack slot ??? */
      for (k = 0; k < slots_num; k++) {
        if (k == 0) best_loc = VARR_LENGTH (size_t, loc_profits);
        VARR_PUSH (size_t, loc_profits, 0);
        VARR_PUSH (size_t, loc_profit_ages, 0);
        if (k == 0 && (best_loc - MAX_HARD_REG - 1) % slots_num != 0) k--; /* align */
      }
      func_stack_slots_num = VARR_LENGTH (size_t, loc_profits) - MAX_HARD_REG - 1;
    }
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      MIR_reg_t thread_breg = curr_breg_infos[breg].thread_first;

      fprintf (debug_file,
               " Assigning to %s:var=%3u, breg=%3u (freq %-3ld), thread breg=%3u (freq %-3ld) -- "
               "%lu\n",
               MIR_reg_name (ctx, reg, curr_func_item->u.func), reg2var (gen_ctx, reg), breg,
               curr_breg_infos[breg].freq, thread_breg, curr_breg_infos[thread_breg].thread_freq,
               (unsigned long) best_loc);
    }
#endif
    VARR_SET (MIR_reg_t, breg_renumber, breg, best_loc);
    for (lr = VARR_GET (live_range_t, var_live_ranges, var); lr != NULL; lr = lr->next)
      for (j = lr->start; j <= lr->finish; j++)
        for (k = 0; k < slots_num; k++) bitmap_set_bit_p (point_used_locs_addr[j], best_loc + k);
  }
  for (i = 0; i <= curr_point; i++) bitmap_destroy (VARR_POP (bitmap_t, point_used_locs));
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    fprintf (debug_file, "+++++++++++++Disposition after assignment:");
    for (i = 0; i < nregs; i++) {
      if (i % 8 == 0) fprintf (debug_file, "\n");
      reg = breg2reg (gen_ctx, i);
      fprintf (debug_file, " %3u=>%-2u", reg2var (gen_ctx, reg),
               VARR_GET (MIR_reg_t, breg_renumber, i));
    }
    fprintf (debug_file, "\n");
  }
#endif
}

static MIR_reg_t change_reg (MIR_context_t ctx, MIR_op_t *mem_op, MIR_reg_t reg,
                             MIR_op_mode_t data_mode, int first_p, bb_insn_t bb_insn, int out_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t loc = VARR_GET (MIR_reg_t, breg_renumber, reg2breg (gen_ctx, reg));
  MIR_reg_t hard_reg;
  MIR_disp_t offset;
  MIR_insn_code_t code;
  MIR_type_t type;
  MIR_insn_t insn;
  bb_insn_t new_bb_insn;
  MIR_op_t hard_reg_op;

  gen_assert (loc != MIR_NON_HARD_REG);
  if (loc <= MAX_HARD_REG) return loc;
  gen_assert (data_mode == MIR_OP_INT || data_mode == MIR_OP_FLOAT || data_mode == MIR_OP_DOUBLE
              || data_mode == MIR_OP_LDOUBLE);
  if (data_mode == MIR_OP_INT) {
    type = MIR_T_I64;
    code = MIR_MOV;
    hard_reg = first_p ? TEMP_INT_HARD_REG1 : TEMP_INT_HARD_REG2;
  } else if (data_mode == MIR_OP_FLOAT) {
    type = MIR_T_F;
    code = MIR_FMOV;
    hard_reg = first_p ? TEMP_FLOAT_HARD_REG1 : TEMP_FLOAT_HARD_REG2;
  } else if (data_mode == MIR_OP_DOUBLE) {
    type = MIR_T_D;
    code = MIR_DMOV;
    hard_reg = first_p ? TEMP_DOUBLE_HARD_REG1 : TEMP_DOUBLE_HARD_REG2;
  } else {
    type = MIR_T_LD;
    code = MIR_LDMOV;
    hard_reg = first_p ? TEMP_LDOUBLE_HARD_REG1 : TEMP_LDOUBLE_HARD_REG2;
  }
  offset = target_get_stack_slot_offset (ctx, type, loc - MAX_HARD_REG - 1);
  *mem_op = _MIR_new_hard_reg_mem_op (ctx, type, offset, FP_HARD_REG, MIR_NON_HARD_REG, 0);
  if (hard_reg == MIR_NON_HARD_REG) return hard_reg;
  hard_reg_op = _MIR_new_hard_reg_op (ctx, hard_reg);
  if (out_p) {
    insn = MIR_new_insn (ctx, code, *mem_op, hard_reg_op);
    MIR_insert_insn_after (ctx, curr_func_item, bb_insn->insn, insn);
  } else {
    insn = MIR_new_insn (ctx, code, hard_reg_op, *mem_op);
    MIR_insert_insn_before (ctx, curr_func_item, bb_insn->insn, insn);
  }
  new_bb_insn = create_bb_insn (ctx, insn, bb_insn->bb);
  if (out_p)
    DLIST_INSERT_AFTER (bb_insn_t, bb_insn->bb->bb_insns, bb_insn, new_bb_insn);
  else
    DLIST_INSERT_BEFORE (bb_insn_t, bb_insn->bb->bb_insns, bb_insn, new_bb_insn);
  return hard_reg;
}

static void rewrite (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn;
  bb_insn_t bb_insn, next_bb_insn;
  size_t nops, i;
  MIR_op_t *op, mem_op;
#if !MIR_NO_GEN_DEBUG
  MIR_op_t in_op = MIR_new_int_op (ctx, 0),
           out_op = MIR_new_int_op (ctx, 0); /* To remove unitilized warning */
#endif
  MIR_mem_t mem;
  MIR_op_mode_t data_mode;
  MIR_reg_t hard_reg;
  int out_p, first_in_p;
  size_t insns_num = 0, movs_num = 0, deleted_movs_num = 0;

  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    for (bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns); bb_insn != NULL; bb_insn = next_bb_insn) {
      next_bb_insn = DLIST_NEXT (bb_insn_t, bb_insn);
      insn = bb_insn->insn;
      nops = MIR_insn_nops (ctx, insn);
      first_in_p = TRUE;
      for (i = 0; i < nops; i++) {
        op = &insn->ops[i];
        data_mode = MIR_insn_op_mode (ctx, insn, i, &out_p);
#if !MIR_NO_GEN_DEBUG
        if (out_p)
          out_op = *op; /* we don't care about multiple call outputs here */
        else
          in_op = *op;
#endif
        switch (op->mode) {
        case MIR_OP_REG:
          hard_reg
            = change_reg (ctx, &mem_op, op->u.reg, data_mode, out_p || first_in_p, bb_insn, out_p);
          if (!out_p) first_in_p = FALSE;
          if (hard_reg == MIR_NON_HARD_REG) {
            *op = mem_op;
          } else {
            op->mode = MIR_OP_HARD_REG;
            op->u.hard_reg = hard_reg;
          }
          break;
        case MIR_OP_MEM:
          mem = op->u.mem;
          /* Always second for mov MEM[R2], R1 or mov R1, MEM[R2]. */
          if (op->u.mem.base == 0) {
            mem.base = MIR_NON_HARD_REG;
          } else {
            mem.base = change_reg (ctx, &mem_op, op->u.mem.base, MIR_OP_INT, FALSE, bb_insn, FALSE);
            gen_assert (mem.base != MIR_NON_HARD_REG); /* we can always use GP regs */
          }
          gen_assert (op->u.mem.index == 0);
          mem.index = MIR_NON_HARD_REG;
          op->mode = MIR_OP_HARD_REG_MEM;
          op->u.hard_reg_mem = mem;
          break;
        default: /* do nothing */ break;
        }
      }
      insns_num++;
      if (insn->code == MIR_MOV || insn->code == MIR_FMOV || insn->code == MIR_DMOV
          || insn->code == MIR_LDMOV) {
        movs_num++;
        if (MIR_op_eq_p (ctx, insn->ops[0], insn->ops[1])) {
#if !MIR_NO_GEN_DEBUG
          if (debug_file != NULL) {
            fprintf (debug_file, "Deleting noop move ");
            MIR_output_insn (ctx, debug_file, insn, curr_func_item->u.func, FALSE);
            fprintf (debug_file, " which was ");
            insn->ops[0] = out_op;
            insn->ops[1] = in_op;
            MIR_output_insn (ctx, debug_file, insn, curr_func_item->u.func, TRUE);
          }
#endif
          deleted_movs_num++;
          gen_delete_insn (ctx, insn);
        }
      }
    }
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL)
    fprintf (debug_file,
             "Deleting moves: %lu deleted noop moves out of %lu non-conflicting moves (%.1f%%), "
             "out of %lu all moves (%.1f), out of %lu all insns (%.1f)\n",
             (unsigned long) deleted_movs_num, (unsigned long) curr_cfg->non_conflicting_moves,
             deleted_movs_num * 100.0 / curr_cfg->non_conflicting_moves, (unsigned long) movs_num,
             deleted_movs_num * 100.0 / movs_num, (unsigned long) insns_num,
             deleted_movs_num * 100.0 / insns_num);
#endif
}

static void init_ra (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  gen_ctx->ra_ctx = gen_malloc (ctx, sizeof (struct ra_ctx));
  VARR_CREATE (MIR_reg_t, breg_renumber, 0);
  VARR_CREATE (breg_info_t, sorted_bregs, 0);
  VARR_CREATE (bitmap_t, point_used_locs, 0);
  VARR_CREATE (size_t, loc_profits, 0);
  VARR_CREATE (size_t, loc_profit_ages, 0);
  conflict_locs = bitmap_create2 (3 * MAX_HARD_REG / 2);
  func_assigned_hard_regs = bitmap_create2 (MAX_HARD_REG + 1);
}

static void finish_ra (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (MIR_reg_t, breg_renumber);
  VARR_DESTROY (breg_info_t, sorted_bregs);
  VARR_DESTROY (bitmap_t, point_used_locs);
  VARR_DESTROY (size_t, loc_profits);
  VARR_DESTROY (size_t, loc_profit_ages);
  bitmap_destroy (conflict_locs);
  bitmap_destroy (func_assigned_hard_regs);
  free (gen_ctx->ra_ctx);
  gen_ctx->ra_ctx = NULL;
}

/* New Page */

/* Code selection */

struct hreg_ref { /* We keep track of the last hard reg ref in this struct. */
  MIR_insn_t insn;
  size_t insn_num;
  size_t nop;
  char def_p, del_p; /* def/use and deleted */
};

typedef struct hreg_ref hreg_ref_t;

DEF_VARR (hreg_ref_t);
DEF_VARR (MIR_op_t);

struct selection_ctx {
  VARR (size_t) * hreg_ref_ages;
  VARR (hreg_ref_t) * hreg_refs;
  hreg_ref_t *hreg_refs_addr;
  size_t *hreg_ref_ages_addr;
  size_t curr_bb_hreg_ref_age;
  size_t last_mem_ref_insn_num;
  VARR (MIR_reg_t) * insn_hard_regs; /* registers considered for substitution */
  VARR (size_t) * changed_op_numbers;
  VARR (MIR_op_t) * last_right_ops;
  bitmap_t hard_regs_bitmap;
};

#define hreg_ref_ages gen_ctx->selection_ctx->hreg_ref_ages
#define hreg_refs gen_ctx->selection_ctx->hreg_refs
#define hreg_refs_addr gen_ctx->selection_ctx->hreg_refs_addr
#define hreg_ref_ages_addr gen_ctx->selection_ctx->hreg_ref_ages_addr
#define curr_bb_hreg_ref_age gen_ctx->selection_ctx->curr_bb_hreg_ref_age
#define last_mem_ref_insn_num gen_ctx->selection_ctx->last_mem_ref_insn_num
#define insn_hard_regs gen_ctx->selection_ctx->insn_hard_regs
#define changed_op_numbers gen_ctx->selection_ctx->changed_op_numbers
#define last_right_ops gen_ctx->selection_ctx->last_right_ops
#define hard_regs_bitmap gen_ctx->selection_ctx->hard_regs_bitmap

static MIR_insn_code_t commutative_insn_code (MIR_insn_code_t insn_code) {
  switch (insn_code) {
  case MIR_ADD:
  case MIR_ADDS:
  case MIR_FADD:
  case MIR_DADD:
  case MIR_LDADD:
  case MIR_MUL:
  case MIR_MULS:
  case MIR_FMUL:
  case MIR_DMUL:
  case MIR_LDMUL:
  case MIR_AND:
  case MIR_OR:
  case MIR_XOR:
  case MIR_ANDS:
  case MIR_ORS:
  case MIR_XORS:
  case MIR_EQ:
  case MIR_EQS:
  case MIR_FEQ:
  case MIR_DEQ:
  case MIR_LDEQ:
  case MIR_NE:
  case MIR_NES:
  case MIR_FNE:
  case MIR_DNE:
  case MIR_LDNE:
  case MIR_BEQ:
  case MIR_BEQS:
  case MIR_FBEQ:
  case MIR_DBEQ:
  case MIR_LDBEQ:
  case MIR_BNE:
  case MIR_BNES:
  case MIR_FBNE:
  case MIR_DBNE:
  case MIR_LDBNE: return insn_code; break;
  case MIR_LT: return MIR_GT;
  case MIR_LTS: return MIR_GTS;
  case MIR_ULT: return MIR_UGT;
  case MIR_ULTS: return MIR_UGTS;
  case MIR_LE: return MIR_GE;
  case MIR_LES: return MIR_GES;
  case MIR_ULE: return MIR_UGE;
  case MIR_ULES: return MIR_UGES;
  case MIR_GT: return MIR_LT;
  case MIR_GTS: return MIR_LTS;
  case MIR_UGT: return MIR_ULT;
  case MIR_UGTS: return MIR_ULTS;
  case MIR_GE: return MIR_LE;
  case MIR_GES: return MIR_LES;
  case MIR_UGE: return MIR_ULE;
  case MIR_UGES: return MIR_ULES;
  case MIR_BLT: return MIR_BGT;
  case MIR_BLTS: return MIR_BGTS;
  case MIR_UBLT: return MIR_UBGT;
  case MIR_UBLTS: return MIR_UBGTS;
  case MIR_BLE: return MIR_BGE;
  case MIR_BLES: return MIR_BGES;
  case MIR_UBLE: return MIR_UBGE;
  case MIR_UBLES: return MIR_UBGES;
  case MIR_BGT: return MIR_BLT;
  case MIR_BGTS: return MIR_BLTS;
  case MIR_UBGT: return MIR_UBLT;
  case MIR_UBGTS: return MIR_UBLTS;
  case MIR_BGE: return MIR_BLE;
  case MIR_BGES: return MIR_BLES;
  case MIR_UBGE: return MIR_UBLE;
  case MIR_UBGES: return MIR_UBLES;
  case MIR_FLT: return MIR_FGT;
  case MIR_DLT: return MIR_DGT;
  case MIR_LDLT: return MIR_LDGT;
  case MIR_FLE: return MIR_FGE;
  case MIR_DLE: return MIR_DGE;
  case MIR_LDLE: return MIR_LDGE;
  case MIR_FGT: return MIR_FLT;
  case MIR_DGT: return MIR_DLT;
  case MIR_LDGT: return MIR_LDLT;
  case MIR_FGE: return MIR_FLE;
  case MIR_DGE: return MIR_DLE;
  case MIR_LDGE: return MIR_LDLE;
  case MIR_FBLT: return MIR_FBGT;
  case MIR_DBLT: return MIR_DBGT;
  case MIR_LDBLT: return MIR_LDBGT;
  case MIR_FBLE: return MIR_FBGE;
  case MIR_DBLE: return MIR_DBGE;
  case MIR_LDBLE: return MIR_LDBGE;
  case MIR_FBGT: return MIR_FBLT;
  case MIR_DBGT: return MIR_DBLT;
  case MIR_LDBGT: return MIR_LDBLT;
  case MIR_FBGE: return MIR_FBLE;
  case MIR_DBGE: return MIR_DBLE;
  case MIR_LDBGE: return MIR_LDBLE;
  default: return MIR_INSN_BOUND;
  }
}

static int obsolete_hard_reg_p (MIR_context_t ctx, MIR_reg_t hard_reg, size_t def_insn_num) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  return (hreg_ref_ages_addr[hard_reg] == curr_bb_hreg_ref_age
          && hreg_refs_addr[hard_reg].insn_num > def_insn_num);
}

static int obsolete_hard_reg_op_p (MIR_context_t ctx, MIR_op_t op, size_t def_insn_num) {
  return op.mode == MIR_OP_HARD_REG && obsolete_hard_reg_p (ctx, op.u.hard_reg, def_insn_num);
}

static int obsolete_op_p (MIR_context_t ctx, MIR_op_t op, size_t def_insn_num) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (obsolete_hard_reg_op_p (ctx, op, def_insn_num)) return TRUE;
  if (op.mode != MIR_OP_HARD_REG_MEM) return FALSE;
  if (op.u.hard_reg_mem.base != MIR_NON_HARD_REG
      && obsolete_hard_reg_p (ctx, op.u.hard_reg_mem.base, def_insn_num))
    return TRUE;
  if (op.u.hard_reg_mem.index != MIR_NON_HARD_REG
      && obsolete_hard_reg_p (ctx, op.u.hard_reg_mem.index, def_insn_num))
    return TRUE;
  return last_mem_ref_insn_num > def_insn_num;
}

static int safe_hreg_substitution_p (MIR_context_t ctx, MIR_reg_t hr, bb_insn_t bb_insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  return (hr != MIR_NON_HARD_REG
          && hreg_ref_ages_addr[hr] == curr_bb_hreg_ref_age
          /* It is not safe to substitute if there is another use after def insn before
             the current insn as we delete def insn after the substitution. */
          && hreg_refs_addr[hr].def_p && find_bb_insn_dead_var (bb_insn, hr) != NULL);
}

static void combine_process_hard_reg (MIR_context_t ctx, MIR_reg_t hr, bb_insn_t bb_insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (!safe_hreg_substitution_p (ctx, hr, bb_insn) || !bitmap_set_bit_p (hard_regs_bitmap, hr))
    return;
  VARR_PUSH (MIR_reg_t, insn_hard_regs, hr);
}

static void combine_process_op (MIR_context_t ctx, const MIR_op_t *op_ref, bb_insn_t bb_insn) {
  if (op_ref->mode == MIR_OP_HARD_REG) {
    combine_process_hard_reg (ctx, op_ref->u.hard_reg, bb_insn);
  } else if (op_ref->mode == MIR_OP_HARD_REG_MEM) {
    if (op_ref->u.hard_reg_mem.base != MIR_NON_HARD_REG)
      combine_process_hard_reg (ctx, op_ref->u.hard_reg_mem.base, bb_insn);
    if (op_ref->u.hard_reg_mem.index != MIR_NON_HARD_REG)
      combine_process_hard_reg (ctx, op_ref->u.hard_reg_mem.index, bb_insn);
  }
}

static void combine_delete_insn (MIR_context_t ctx, MIR_insn_t def_insn, bb_insn_t bb_insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_reg_t hr;

  gen_assert (def_insn->ops[0].mode == MIR_OP_HARD_REG);
  hr = def_insn->ops[0].u.hard_reg;
  if (hreg_ref_ages_addr[hr] != curr_bb_hreg_ref_age || hreg_refs_addr[hr].del_p) return;
#if !MIR_NO_GEN_DEBUG
  // deleted_insns_num++;
  if (debug_file != NULL) {
    fprintf (debug_file, "      deleting now dead insn ");
    print_bb_insn (ctx, def_insn->data, TRUE);
  }
#endif
  remove_bb_insn_dead_var (bb_insn, hr);
  move_bb_insn_dead_vars (bb_insn, def_insn->data);
  /* We should delete the def insn here because of possible
     substitution of the def insn 'r0 = ... r0 ...'.  We still
     need valid entry for def here to find obsolete definiton,
     e.g. "hr1 = hr0; hr0 = ...; hr0 = ... (deleted); ...= ...hr1..." */
  gen_delete_insn (ctx, def_insn);
  hreg_refs_addr[hr].del_p = TRUE; /* to exclude repetitive deletion */
}

static int64_t power2 (int64_t p) {
  int64_t n = 1;

  if (p < 0) return 0;
  while (p-- > 0) n *= 2;
  return n;
}

static int64_t int_log2 (int64_t i) {
  int64_t n;

  if (i == 0) return -1;
  for (n = 0; (i & 1) == 0; n++, i >>= 1)
    ;
  return i == 1 ? n : -1;
}

static int combine_substitute (MIR_context_t ctx, bb_insn_t bb_insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_code_t code, new_code;
  MIR_insn_t insn = bb_insn->insn, def_insn, new_insn;
  size_t i, nops = insn->nops;
  int out_p, insn_change_p, insn_hr_change_p, op_change_p, mem_reg_change_p, success_p;
  MIR_op_t *op_ref, *src_op_ref, *src_op2_ref;
  MIR_reg_t hr;
  int64_t scale, sh;

  if (nops == 0) return FALSE;
  VARR_TRUNC (MIR_op_t, last_right_ops, 0);
  for (i = 0; i < nops; i++) VARR_PUSH (MIR_op_t, last_right_ops, insn->ops[i]);
  VARR_TRUNC (MIR_reg_t, insn_hard_regs, 0);
  bitmap_clear (hard_regs_bitmap);
  for (i = 0; i < nops; i++) {
    MIR_insn_op_mode (ctx, insn, i, &out_p);
    if (out_p && insn->ops[i].mode != MIR_OP_HARD_REG_MEM) continue;
    combine_process_op (ctx, &insn->ops[i], bb_insn);
  }
  insn_change_p = FALSE;
  while (VARR_LENGTH (MIR_reg_t, insn_hard_regs) != 0) {
    hr = VARR_POP (MIR_reg_t, insn_hard_regs);
    if (!hreg_refs_addr[hr].def_p) continue;
    gen_assert (!hreg_refs_addr[hr].del_p);
    def_insn = hreg_refs_addr[hr].insn;
    if ((def_insn->nops > 1 && obsolete_op_p (ctx, def_insn->ops[1], hreg_refs_addr[hr].insn_num))
        || (def_insn->nops > 2
            && obsolete_op_p (ctx, def_insn->ops[2], hreg_refs_addr[hr].insn_num)))
      continue; /* hr0 = ... hr1 ...; ...; hr1 = ...; ...; insn */
    insn_hr_change_p = FALSE;
    for (i = 0; i < nops; i++) { /* Change all hr occurences: */
      op_ref = &insn->ops[i];
      op_change_p = FALSE;
      MIR_insn_op_mode (ctx, insn, i, &out_p);
      if (!out_p && op_ref->mode == MIR_OP_HARD_REG && op_ref->u.hard_reg == hr) {
        if (def_insn->code != MIR_MOV && def_insn->code != MIR_FMOV && def_insn->code != MIR_DMOV
            && def_insn->code != MIR_LDMOV)
          break;
        /* It is not safe to substitute if there is another use after def insn before
           the current as we delete def insn after substitution. */
        insn->ops[i] = def_insn->ops[1];
        insn_hr_change_p = op_change_p = TRUE;
      } else if (op_ref->mode == MIR_OP_HARD_REG_MEM) {
        src_op_ref = &def_insn->ops[1];
        if (op_ref->u.hard_reg_mem.index == hr) {
          mem_reg_change_p = FALSE;
          if (src_op_ref->mode != MIR_OP_HARD_REG) {
          } else if (def_insn->code == MIR_MOV) { /* index = r */
            insn->ops[i].u.hard_reg_mem.index = src_op_ref->u.hard_reg;
            mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
          } else if (def_insn->code == MIR_ADD) { /* index = r + const */
            gen_assert (src_op_ref->u.hard_reg != MIR_NON_HARD_REG);
            if ((src_op2_ref = &def_insn->ops[2])->mode == MIR_OP_INT) {
              insn->ops[i].u.hard_reg_mem.index = src_op_ref->u.hard_reg;
              insn->ops[i].u.hard_reg_mem.disp
                += src_op2_ref->u.i * insn->ops[i].u.hard_reg_mem.scale;
              mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
            }
          } else if ((def_insn->code == MIR_MUL || def_insn->code == MIR_LSH)
                     && op_ref->u.mem.scale >= 1 && op_ref->u.mem.scale <= MIR_MAX_SCALE
                     && (src_op2_ref = &def_insn->ops[2])->mode == MIR_OP_INT) {
            scale = def_insn->code == MIR_MUL ? src_op2_ref->u.i : power2 (src_op2_ref->u.i);
            if (scale >= 1 && scale <= MIR_MAX_SCALE
                && insn->ops[i].u.hard_reg_mem.scale * scale <= MIR_MAX_SCALE) {
              /* index = r * const */
              gen_assert (src_op_ref->u.hard_reg != MIR_NON_HARD_REG);
              insn->ops[i].u.hard_reg_mem.index = src_op_ref->u.hard_reg;
              insn->ops[i].u.hard_reg_mem.scale *= scale;
              mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
            }
          }
          if (!mem_reg_change_p) break;
        }
        if (op_ref->u.hard_reg_mem.base == hr) {
          mem_reg_change_p = FALSE;
          op_ref = &insn->ops[i];
          if (def_insn->code == MIR_MOV) {
            if (src_op_ref->mode == MIR_OP_HARD_REG) { /* base = r */
              insn->ops[i].u.hard_reg_mem.base = src_op_ref->u.hard_reg;
              mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
            } else if (src_op_ref->mode == MIR_OP_INT) { /* base = const */
              if (insn->ops[i].u.hard_reg_mem.scale != 1) {
                insn->ops[i].u.hard_reg_mem.base = MIR_NON_HARD_REG;
              } else {
                insn->ops[i].u.hard_reg_mem.base = insn->ops[i].u.hard_reg_mem.index;
                insn->ops[i].u.hard_reg_mem.index = MIR_NON_HARD_REG;
              }
              insn->ops[i].u.hard_reg_mem.disp += src_op_ref->u.i;
              mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
            }
          } else if (src_op_ref->mode != MIR_OP_HARD_REG) { /* Can do nothing */
            ;
          } else if (def_insn->code == MIR_ADD) {
            gen_assert (src_op_ref->u.hard_reg != MIR_NON_HARD_REG);
            src_op2_ref = &def_insn->ops[2];
            if (src_op2_ref->mode == MIR_OP_HARD_REG
                && op_ref->u.hard_reg_mem.index == MIR_NON_HARD_REG) { /* base = r1 + r2 */
              insn->ops[i].u.hard_reg_mem.base = src_op_ref->u.hard_reg;
              insn->ops[i].u.hard_reg_mem.index = src_op2_ref->u.hard_reg;
              insn->ops[i].u.hard_reg_mem.scale = 1;
              mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
            } else if (src_op2_ref->mode == MIR_OP_INT) { /* base = r + const */
              insn->ops[i].u.hard_reg_mem.base = src_op_ref->u.hard_reg;
              insn->ops[i].u.hard_reg_mem.disp += src_op2_ref->u.i;
              mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
            }
          } else if (def_insn->code == MIR_MUL && op_ref->u.hard_reg_mem.index == MIR_NON_HARD_REG
                     && (src_op2_ref = &def_insn->ops[2])->mode == MIR_OP_INT
                     && src_op2_ref->u.i >= 1
                     && src_op2_ref->u.i <= MIR_MAX_SCALE) { /* base = r * const */
            gen_assert (src_op_ref->u.hard_reg != MIR_NON_HARD_REG && src_op2_ref->u.i != 1);
            insn->ops[i].u.hard_reg_mem.base = MIR_NON_HARD_REG;
            insn->ops[i].u.hard_reg_mem.index = src_op_ref->u.hard_reg;
            insn->ops[i].u.hard_reg_mem.scale = src_op2_ref->u.i;
            mem_reg_change_p = op_change_p = insn_hr_change_p = TRUE;
          }
          if (!mem_reg_change_p) {
            if (op_change_p) VARR_PUSH (size_t, changed_op_numbers, i); /* index was changed */
            break;
          }
        }
      }
      if (op_change_p) VARR_PUSH (size_t, changed_op_numbers, i);
    }
    code = insn->code;
    if (i >= nops && (code == MIR_MUL || code == MIR_MULS || code == MIR_UDIV || code == MIR_UDIVS)
        && insn->ops[2].mode == MIR_OP_INT && (sh = int_log2 (insn->ops[2].u.i)) >= 0) {
      new_code = code; /* to remove an initialized warning */
      switch (code) {
      case MIR_MUL: new_code = MIR_LSH; break;
      case MIR_MULS: new_code = MIR_LSHS; break;
      case MIR_UDIV: new_code = MIR_URSH; break;
      case MIR_UDIVS: new_code = MIR_URSHS; break;
      default: gen_assert (FALSE);
      }
      new_insn = MIR_new_insn (ctx, new_code, insn->ops[0], insn->ops[1], MIR_new_int_op (ctx, sh));
      MIR_insert_insn_after (ctx, curr_func_item, insn, new_insn);
      if (target_insn_ok_p (ctx, new_insn)) {
        insn->code = new_insn->code;
        insn->ops[0] = new_insn->ops[0];
        insn->ops[1] = new_insn->ops[1];
        insn->ops[2] = new_insn->ops[2];
      }
      MIR_remove_insn (ctx, curr_func_item, new_insn);
      insn_hr_change_p = TRUE;
    }
    if (insn_hr_change_p) {
      if ((success_p = i >= nops && target_insn_ok_p (ctx, insn))) insn_change_p = TRUE;
      while (VARR_LENGTH (size_t, changed_op_numbers)) {
        i = VARR_POP (size_t, changed_op_numbers);
        if (success_p)
          VARR_SET (MIR_op_t, last_right_ops, i, insn->ops[i]);
        else
          insn->ops[i] = VARR_GET (MIR_op_t, last_right_ops, i); /* restore changed operands */
      }
      if (success_p) {
        gen_assert (def_insn != NULL);
        combine_delete_insn (ctx, def_insn, bb_insn);
#if !MIR_NO_GEN_DEBUG
        if (debug_file != NULL) {
          fprintf (debug_file, "      changing to ");
          print_bb_insn (ctx, bb_insn, TRUE);
        }
#endif
      }
    }
  }
  return insn_change_p;
}

static MIR_insn_code_t get_combined_br_code (int true_p, MIR_insn_code_t cmp_code) {
  switch (cmp_code) {
  case MIR_EQ: return true_p ? MIR_BEQ : MIR_BNE;
  case MIR_EQS: return true_p ? MIR_BEQS : MIR_BNES;
  case MIR_NE: return true_p ? MIR_BNE : MIR_BEQ;
  case MIR_NES: return true_p ? MIR_BNES : MIR_BEQS;
  case MIR_LT: return true_p ? MIR_BLT : MIR_BGE;
  case MIR_LTS: return true_p ? MIR_BLTS : MIR_BGES;
  case MIR_ULT: return true_p ? MIR_UBLT : MIR_UBGE;
  case MIR_ULTS: return true_p ? MIR_UBLTS : MIR_UBGES;
  case MIR_LE: return true_p ? MIR_BLE : MIR_BGT;
  case MIR_LES: return true_p ? MIR_BLES : MIR_BGTS;
  case MIR_ULE: return true_p ? MIR_UBLE : MIR_UBGT;
  case MIR_ULES: return true_p ? MIR_UBLES : MIR_UBGTS;
  case MIR_GT: return true_p ? MIR_BGT : MIR_BLE;
  case MIR_GTS: return true_p ? MIR_BGTS : MIR_BLES;
  case MIR_UGT: return true_p ? MIR_UBGT : MIR_UBLE;
  case MIR_UGTS: return true_p ? MIR_UBGTS : MIR_UBLES;
  case MIR_GE: return true_p ? MIR_BGE : MIR_BLT;
  case MIR_GES: return true_p ? MIR_BGES : MIR_BLTS;
  case MIR_UGE: return true_p ? MIR_UBGE : MIR_UBLT;
  case MIR_UGES:
    return true_p ? MIR_UBGES : MIR_UBLTS;
    /* Cannot revert in the false case for IEEE754: */
  case MIR_FEQ: return true_p ? MIR_FBEQ : MIR_INSN_BOUND;
  case MIR_DEQ: return true_p ? MIR_DBEQ : MIR_INSN_BOUND;
  case MIR_LDEQ: return true_p ? MIR_LDBEQ : MIR_INSN_BOUND;
  case MIR_FNE: return true_p ? MIR_FBNE : MIR_INSN_BOUND;
  case MIR_DNE: return true_p ? MIR_DBNE : MIR_INSN_BOUND;
  case MIR_LDNE: return true_p ? MIR_LDBNE : MIR_INSN_BOUND;
  case MIR_FLT: return true_p ? MIR_FBLT : MIR_INSN_BOUND;
  case MIR_DLT: return true_p ? MIR_DBLT : MIR_INSN_BOUND;
  case MIR_LDLT: return true_p ? MIR_LDBLT : MIR_INSN_BOUND;
  case MIR_FLE: return true_p ? MIR_FBLE : MIR_INSN_BOUND;
  case MIR_DLE: return true_p ? MIR_DBLE : MIR_INSN_BOUND;
  case MIR_LDLE: return true_p ? MIR_LDBLE : MIR_INSN_BOUND;
  case MIR_FGT: return true_p ? MIR_FBGT : MIR_INSN_BOUND;
  case MIR_DGT: return true_p ? MIR_DBGT : MIR_INSN_BOUND;
  case MIR_LDGT: return true_p ? MIR_LDBGT : MIR_INSN_BOUND;
  case MIR_FGE: return true_p ? MIR_FBGE : MIR_INSN_BOUND;
  case MIR_DGE: return true_p ? MIR_DBGE : MIR_INSN_BOUND;
  case MIR_LDGE: return true_p ? MIR_LDBGE : MIR_INSN_BOUND;
  default: return MIR_INSN_BOUND;
  }
}

static MIR_insn_t combine_branch_and_cmp (MIR_context_t ctx, bb_insn_t bb_insn) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t def_insn, new_insn, insn = bb_insn->insn;
  MIR_insn_code_t code = insn->code;
  MIR_op_t op;

  if (code != MIR_BT && code != MIR_BF && code != MIR_BTS && code != MIR_BFS) return NULL;
  op = insn->ops[1];
  if (op.mode != MIR_OP_HARD_REG || !safe_hreg_substitution_p (ctx, op.u.hard_reg, bb_insn))
    return NULL;
  def_insn = hreg_refs_addr[op.u.hard_reg].insn;
  if ((code = get_combined_br_code (code == MIR_BT || code == MIR_BTS, def_insn->code))
      == MIR_INSN_BOUND)
    return NULL;
  if (obsolete_op_p (ctx, def_insn->ops[1], hreg_refs_addr[op.u.hard_reg].insn_num)
      || obsolete_op_p (ctx, def_insn->ops[2], hreg_refs_addr[op.u.hard_reg].insn_num))
    return NULL;
  new_insn = MIR_new_insn (ctx, code, insn->ops[0], def_insn->ops[1], def_insn->ops[2]);
  MIR_insert_insn_before (ctx, curr_func_item, insn, new_insn);
  if (!target_insn_ok_p (ctx, new_insn)) {
    MIR_remove_insn (ctx, curr_func_item, new_insn);
    return NULL;
  } else {
    MIR_remove_insn (ctx, curr_func_item, insn);
    new_insn->data = bb_insn;
    bb_insn->insn = new_insn;
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "      changing to ");
      print_bb_insn (ctx, bb_insn, TRUE);
    }
#endif
    combine_delete_insn (ctx, def_insn, bb_insn);
    return new_insn;
  }
}

static void setup_hreg_ref (MIR_context_t ctx, MIR_reg_t hr, MIR_insn_t insn, size_t nop,
                            size_t insn_num, int def_p) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (hr == MIR_NON_HARD_REG) return;
  hreg_ref_ages_addr[hr] = curr_bb_hreg_ref_age;
  hreg_refs_addr[hr].insn = insn;
  hreg_refs_addr[hr].nop = nop;
  hreg_refs_addr[hr].insn_num = insn_num;
  hreg_refs_addr[hr].def_p = def_p;
  hreg_refs_addr[hr].del_p = FALSE;
}

static void combine (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_code_t code, new_code;
  MIR_insn_t insn, new_insn;
  bb_insn_t bb_insn;
  size_t iter, nops, i, curr_insn_num;
  MIR_op_t temp_op, *op_ref;
  MIR_reg_t early_clobbered_hard_reg1, early_clobbered_hard_reg2;
  int out_p, change_p, block_change_p;
#if !MIR_NO_GEN_DEBUG
  size_t insns_num = 0, deleted_insns_num = 0;
#endif

  hreg_refs_addr = VARR_ADDR (hreg_ref_t, hreg_refs);
  hreg_ref_ages_addr = VARR_ADDR (size_t, hreg_ref_ages);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    do {
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) fprintf (debug_file, "Processing bb%lu\n", (unsigned long) bb->index);
#endif
      block_change_p = FALSE;
      curr_bb_hreg_ref_age++;
      last_mem_ref_insn_num = 0; /* means undef */
      for (bb_insn = DLIST_HEAD (bb_insn_t, bb->bb_insns), curr_insn_num = 1; bb_insn != NULL;
           bb_insn = DLIST_NEXT (bb_insn_t, bb_insn), curr_insn_num++) {
        insn = bb_insn->insn;
        nops = MIR_insn_nops (ctx, insn);
#if !MIR_NO_GEN_DEBUG
        if (insn->code != MIR_LABEL) insns_num++;
        if (debug_file != NULL) {
          fprintf (debug_file, "  Processing ");
          print_bb_insn (ctx, bb_insn, TRUE);
        }
#endif
        target_get_early_clobbered_hard_regs (insn, &early_clobbered_hard_reg1,
                                              &early_clobbered_hard_reg2);
        if (early_clobbered_hard_reg1 != MIR_NON_HARD_REG)
          setup_hreg_ref (ctx, early_clobbered_hard_reg1, insn, 0 /* whatever */, curr_insn_num,
                          TRUE);
        if (early_clobbered_hard_reg2 != MIR_NON_HARD_REG)
          setup_hreg_ref (ctx, early_clobbered_hard_reg2, insn, 0 /* whatever */, curr_insn_num,
                          TRUE);
        if (MIR_call_code_p (code = insn->code)) {
          for (size_t hr = 0; hr <= MAX_HARD_REG; hr++)
            if (bitmap_bit_p (call_used_hard_regs, hr)) {
              setup_hreg_ref (ctx, hr, insn, 0 /* whatever */, curr_insn_num, TRUE);
            }
        } else if (code == MIR_RET) {
          /* ret is transformed in machinize and should be not modified after that */
        } else if ((new_insn = combine_branch_and_cmp (ctx, bb_insn)) != NULL) {
          insn = new_insn;
          nops = MIR_insn_nops (ctx, insn);
          block_change_p = TRUE;
        } else {
          change_p = combine_substitute (ctx, bb_insn);
          if (!change_p && (new_code = commutative_insn_code (insn->code)) != MIR_INSN_BOUND) {
            insn->code = new_code;
            temp_op = insn->ops[1];
            insn->ops[1] = insn->ops[2];
            insn->ops[2] = temp_op;
            if (combine_substitute (ctx, bb_insn))
              change_p = TRUE;
            else {
              insn->code = code;
              temp_op = insn->ops[1];
              insn->ops[1] = insn->ops[2];
              insn->ops[2] = temp_op;
            }
          }
          if (change_p) block_change_p = TRUE;
          if (code == MIR_BSTART || code == MIR_BEND) last_mem_ref_insn_num = curr_insn_num;
        }

        for (iter = 0; iter < 2; iter++) { /* update hreg ref info: */
          for (i = 0; i < nops; i++) {
            op_ref = &insn->ops[i];
            MIR_insn_op_mode (ctx, insn, i, &out_p);
            if (op_ref->mode == MIR_OP_HARD_REG && !iter == !out_p) {
              /* process in ops on 0th iteration and out ops on 1th iteration */
              setup_hreg_ref (ctx, op_ref->u.hard_reg, insn, i, curr_insn_num, iter == 1);
            } else if (op_ref->mode == MIR_OP_HARD_REG_MEM) {
              if (out_p && iter == 1)
                last_mem_ref_insn_num = curr_insn_num;
              else if (iter == 0) {
                setup_hreg_ref (ctx, op_ref->u.hard_reg_mem.base, insn, i, curr_insn_num, FALSE);
                setup_hreg_ref (ctx, op_ref->u.hard_reg_mem.index, insn, i, curr_insn_num, FALSE);
              }
            }
          }
        }
      }
    } while (block_change_p);
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL)
    fprintf (debug_file, "  %lu deleted out of %lu (%.1f%%)\n", deleted_insns_num, insns_num,
             100.0 * deleted_insns_num / insns_num);
#endif
}

static void init_selection (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  hreg_ref_t hreg_ref = {NULL, 0, 0};

  gen_ctx->selection_ctx = gen_malloc (ctx, sizeof (struct selection_ctx));
  curr_bb_hreg_ref_age = 0;
  VARR_CREATE (size_t, hreg_ref_ages, MAX_HARD_REG + 1);
  VARR_CREATE (hreg_ref_t, hreg_refs, MAX_HARD_REG + 1);
  VARR_CREATE (MIR_reg_t, insn_hard_regs, 0);
  VARR_CREATE (size_t, changed_op_numbers, 16);
  VARR_CREATE (MIR_op_t, last_right_ops, 16);
  hard_regs_bitmap = bitmap_create2 (MAX_HARD_REG + 1);
  for (MIR_reg_t i = 0; i <= MAX_HARD_REG; i++) {
    VARR_PUSH (hreg_ref_t, hreg_refs, hreg_ref);
    VARR_PUSH (size_t, hreg_ref_ages, 0);
  }
}

static void finish_selection (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  VARR_DESTROY (size_t, hreg_ref_ages);
  VARR_DESTROY (hreg_ref_t, hreg_refs);
  VARR_DESTROY (MIR_reg_t, insn_hard_regs);
  VARR_DESTROY (size_t, changed_op_numbers);
  VARR_DESTROY (MIR_op_t, last_right_ops);
  bitmap_destroy (hard_regs_bitmap);
  free (gen_ctx->selection_ctx);
  gen_ctx->selection_ctx = NULL;
}

/* New Page */

/* Dead code elimnination */

#define live_out out

static void dead_code_elimination (MIR_context_t ctx) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  MIR_insn_t insn;
  bb_insn_t bb_insn, prev_bb_insn;
  size_t passed_mem_num;
  MIR_reg_t var, early_clobbered_hard_reg1, early_clobbered_hard_reg2;
  int out_p, reg_def_p, dead_p, mem_p;
  bitmap_t live;
  insn_var_iterator_t insn_var_iter;

#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) fprintf (debug_file, "+++++++++++++Dead code elimination:\n");
#endif
  live = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  for (bb_t bb = DLIST_HEAD (bb_t, curr_cfg->bbs); bb != NULL; bb = DLIST_NEXT (bb_t, bb)) {
    bitmap_copy (live, bb->live_out);
    for (bb_insn = DLIST_TAIL (bb_insn_t, bb->bb_insns); bb_insn != NULL; bb_insn = prev_bb_insn) {
      prev_bb_insn = DLIST_PREV (bb_insn_t, bb_insn);
      insn = bb_insn->insn;
      reg_def_p = FALSE;
      dead_p = TRUE;
      FOREACH_INSN_VAR (ctx, insn_var_iter, insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p) continue;
        reg_def_p = TRUE;
        if (bitmap_clear_bit_p (live, var)) dead_p = FALSE;
      }
      if (!reg_def_p) dead_p = FALSE;
      if (dead_p && !MIR_call_code_p (insn->code) && insn->code != MIR_RET
          && insn->code != MIR_ALLOCA && insn->code != MIR_BSTART && insn->code != MIR_BEND
          && insn->code != MIR_VA_START && insn->code != MIR_VA_ARG && insn->code != MIR_VA_END
          && !(insn->ops[0].mode == MIR_OP_HARD_REG
               && (insn->ops[0].u.hard_reg == FP_HARD_REG
                   || insn->ops[0].u.hard_reg == SP_HARD_REG))) {
#if !MIR_NO_GEN_DEBUG
        if (debug_file != NULL) {
          fprintf (debug_file, "  Removing dead insn");
          MIR_output_insn (ctx, debug_file, insn, curr_func_item->u.func, TRUE);
        }
#endif
        gen_delete_insn (ctx, insn);
        continue;
      }
      if (MIR_call_code_p (insn->code)) bitmap_and_compl (live, live, call_used_hard_regs);
      FOREACH_INSN_VAR (ctx, insn_var_iter, insn, var, out_p, mem_p, passed_mem_num) {
        if (!out_p) bitmap_set_bit_p (live, var);
      }
      target_get_early_clobbered_hard_regs (insn, &early_clobbered_hard_reg1,
                                            &early_clobbered_hard_reg2);
      if (early_clobbered_hard_reg1 != MIR_NON_HARD_REG)
        bitmap_clear_bit_p (live, early_clobbered_hard_reg1);
      if (early_clobbered_hard_reg2 != MIR_NON_HARD_REG)
        bitmap_clear_bit_p (live, early_clobbered_hard_reg2);
      if (MIR_call_code_p (insn->code)) bitmap_ior (live, live, bb_insn->call_hard_reg_args);
    }
  }
  bitmap_destroy (live);
}

#undef live_out

#if !MIR_NO_GEN_DEBUG

#include <sys/types.h>
#include <unistd.h>

static void print_code (MIR_context_t ctx, uint8_t *code, size_t code_len, void *start_addr) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  size_t i;
  int ch;
  char cfname[30];
  char command[500];
  FILE *f;
#if !defined(__APPLE__)
  char bfname[30];
  FILE *bf;
#endif

  sprintf (cfname, "_mir_%lu.c", (unsigned long) getpid ());
  if ((f = fopen (cfname, "w")) == NULL) return;
#if defined(__APPLE__)
  fprintf (f, "unsigned char code[] = {");
  for (i = 0; i < code_len; i++) {
    if (i != 0) fprintf (f, ", ");
    fprintf (f, "0x%x", code[i]);
  }
  fprintf (f, "};\n");
  fclose (f);
  sprintf (command, "gcc -c -o %s.o %s 2>&1 && objdump --section=.data -D %s.o; rm -f %s.o %s",
           cfname, cfname, cfname, cfname, cfname);
#else
  sprintf (bfname, "_mir_%lu.bin", (unsigned long) getpid ());
  if ((bf = fopen (bfname, "w")) == NULL) return;
  fprintf (f, "void code (void) {}\n");
  for (i = 0; i < code_len; i++) fputc (code[i], bf);
  fclose (f);
  fclose (bf);
  sprintf (command,
           "gcc -c -o %s.o %s 2>&1 && objcopy --update-section .text=%s %s.o && objdump "
           "--adjust-vma=0x%lx -d %s.o; rm -f "
           "%s.o %s %s",
           cfname, cfname, bfname, cfname, (unsigned long) start_addr, cfname, cfname, cfname,
           bfname);
#endif
  fprintf (stderr, "%s\n", command);
  if ((f = popen (command, "r")) == NULL) return;
  while ((ch = fgetc (f)) != EOF) fprintf (debug_file, "%c", ch);
  pclose (f);
}
#endif

#if !MIR_NO_GEN_DEBUG
#include <sys/time.h>

static double real_usec_time (void) {
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return tv.tv_usec + tv.tv_sec * 1000000.0;
}
#endif

#if MIR_GEN_CALL_TRACE
static void *print_and_execute_wrapper (MIR_context_t ctx, MIR_item_t called_func) {
  gen_assert (called_func->item_type == MIR_func_item);
  fprintf (stderr, "Calling %s\n", called_func->u.func->name);
  return called_func->u.func->machine_code;
}
#endif

void *MIR_gen (MIR_context_t ctx, MIR_item_t func_item) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);
  uint8_t *code;
  size_t code_len;
#if !MIR_NO_GEN_DEBUG
  double start_time = 0.0;
#endif

  gen_assert (func_item->item_type == MIR_func_item && func_item->data == NULL);
  if (func_item->u.func->machine_code != NULL) {
    gen_assert (func_item->u.func->call_addr != NULL);
    _MIR_redirect_thunk (ctx, func_item->addr, func_item->u.func->call_addr);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL)
      fprintf (debug_file, "+++++++++++++The code for %s has been already generated\n",
               MIR_item_name (ctx, func_item));
#endif
    return func_item->addr;
  }
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    start_time = real_usec_time ();
    fprintf (debug_file, "+++++++++++++MIR before generator:\n");
    MIR_output_item (ctx, debug_file, func_item);
  }
#endif
  curr_func_item = func_item;
  _MIR_duplicate_func_insns (ctx, func_item);
  curr_cfg = func_item->data = gen_malloc (ctx, sizeof (struct func_cfg));
  build_func_cfg (ctx);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    fprintf (debug_file, "+++++++++++++MIR after building CFG:\n");
    print_CFG (ctx, TRUE, FALSE, TRUE, FALSE, NULL);
  }
#endif
#ifndef NO_CSE
  if (optimize_level >= 2) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) fprintf (debug_file, "+++++++++++++CSE:\n");
#endif
    cse (ctx);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      print_exprs (ctx);
      fprintf (debug_file, "+++++++++++++MIR after CSE:\n");
      print_CFG (ctx, TRUE, FALSE, TRUE, FALSE, output_bb_cse_info);
    }
#endif
    cse_clear (ctx);
  }
#endif /* #ifndef NO_CSE */
  calculate_func_cfg_live_info (ctx, FALSE);
#ifndef NO_CSE
  if (optimize_level >= 2) {
    dead_code_elimination (ctx);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "+++++++++++++MIR after dead code elimination after CSE:\n");
      print_CFG (ctx, TRUE, TRUE, TRUE, FALSE, output_bb_live_info);
    }
#endif
  }
#endif /* #ifndef NO_CSE */
#ifndef NO_RENAME
  if (optimize_level >= 3) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) fprintf (debug_file, "+++++++++++++Rename:\n");
#endif
    calculate_reaching_defs (ctx);
    reg_rename (ctx);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "+++++++++++++MIR after rename:\n");
      print_CFG (ctx, TRUE, FALSE, TRUE, TRUE, output_bb_rdef_info);
    }
#endif
    rdef_clear (ctx);
    calculate_func_cfg_live_info (ctx, FALSE); /* restore live info */
  }
#endif /* #ifndef NO_RENAME */
#ifndef NO_LICM
  if (optimize_level >= 3) {
    if (build_loop_tree (ctx)) {
      licm_add_loop_preheaders (ctx, curr_cfg->root_loop_node);
      calculate_reaching_defs (ctx);
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) fprintf (debug_file, "+++++++++++++LICM:\n");
#endif
      licm (ctx);
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "+++++++++++++MIR after LICM:\n");
        print_CFG (ctx, TRUE, FALSE, TRUE, TRUE, output_bb_licm_info);
      }
#endif
      rdef_clear (ctx);
      calculate_func_cfg_live_info (ctx, FALSE); /* restore live info */
    }
    destroy_loop_tree (ctx, curr_cfg->root_loop_node);
    curr_cfg->root_loop_node = NULL;
  }
#endif /* #ifndef NO_LICM */
#ifndef NO_CCP
  if (optimize_level >= 2) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) fprintf (debug_file, "+++++++++++++CCP:\n");
#endif
    if (ccp (ctx)) {
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "+++++++++++++MIR after CCP:\n");
        print_CFG (ctx, TRUE, FALSE, TRUE, FALSE, NULL);
      }
#endif
      dead_code_elimination (ctx);
#if !MIR_NO_GEN_DEBUG
      if (debug_file != NULL) {
        fprintf (debug_file, "+++++++++++++MIR after dead code elimination after CCP:\n");
        print_CFG (ctx, TRUE, TRUE, TRUE, FALSE, output_bb_live_info);
      }
#endif
    }
  }
#endif /* #ifndef NO_CCP */
  ccp_clear (ctx);
  make_io_dup_op_insns (ctx);
  target_machinize (ctx);
  add_new_bb_insns (ctx);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    fprintf (debug_file, "+++++++++++++MIR after machinize:\n");
    print_CFG (ctx, FALSE, FALSE, TRUE, FALSE, NULL);
  }
#endif
  build_loop_tree (ctx);
  calculate_func_cfg_live_info (ctx, TRUE);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    add_bb_insn_dead_vars (ctx);
    fprintf (debug_file, "+++++++++++++MIR after building live_info:\n");
    print_loop_tree (ctx, TRUE);
    print_CFG (ctx, TRUE, TRUE, FALSE, FALSE, output_bb_live_info);
  }
#endif
  build_live_ranges (ctx);
  assign (ctx);
  rewrite (ctx); /* After rewrite the BB live info is still valid */
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    fprintf (debug_file, "+++++++++++++MIR after rewrite:\n");
    print_CFG (ctx, FALSE, FALSE, TRUE, FALSE, NULL);
  }
#endif
  calculate_func_cfg_live_info (ctx, FALSE);
  add_bb_insn_dead_vars (ctx);
#ifndef NO_COMBINE
  if (optimize_level >= 1) {
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "+++++++++++++MIR before combine:\n");
      print_CFG (ctx, FALSE, FALSE, TRUE, FALSE, NULL);
    }
#endif
    combine (ctx); /* After combine the BB live info is still valid */
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "+++++++++++++MIR after combine:\n");
      print_CFG (ctx, FALSE, FALSE, TRUE, FALSE, NULL);
    }
#endif
    dead_code_elimination (ctx);
#if !MIR_NO_GEN_DEBUG
    if (debug_file != NULL) {
      fprintf (debug_file, "+++++++++++++MIR after dead code elimination after combine:\n");
      print_CFG (ctx, TRUE, TRUE, TRUE, FALSE, output_bb_live_info);
    }
#endif
  }
#endif /* #ifndef NO_COMBINE */
  target_make_prolog_epilog (ctx, func_assigned_hard_regs, func_stack_slots_num);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    fprintf (debug_file, "+++++++++++++MIR after forming prolog/epilog:\n");
    print_CFG (ctx, FALSE, FALSE, TRUE, FALSE, NULL);
  }
#endif
  code = target_translate (ctx, &code_len);
  func_item->u.func->machine_code = func_item->u.func->call_addr
    = _MIR_publish_code (ctx, code, code_len);
  target_rebase (ctx, func_item->u.func->machine_code);
#if MIR_GEN_CALL_TRACE
  func_item->u.func->call_addr = _MIR_get_wrapper (ctx, func_item, print_and_execute_wrapper);
#endif
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL) {
    print_code (ctx, func_item->u.func->machine_code, code_len, func_item->u.func->machine_code);
    fprintf (debug_file, "code size = %lu:\n", (unsigned long) code_len);
  }
#endif
  _MIR_redirect_thunk (ctx, func_item->addr, func_item->u.func->call_addr);
  destroy_func_live_ranges (ctx);
  destroy_loop_tree (ctx, curr_cfg->root_loop_node);
  destroy_func_cfg (ctx);
#if !MIR_NO_GEN_DEBUG
  if (debug_file != NULL)
    fprintf (debug_file, "Generation of code for %s -- time %.0f usec\n",
             MIR_item_name (ctx, func_item), real_usec_time () - start_time);
#endif
  _MIR_restore_func_insns (ctx, func_item);
  return func_item->addr;
}

void MIR_gen_set_debug_file (MIR_context_t ctx, FILE *f) {
#if !MIR_NO_GEN_DEBUG
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  if (gen_ctx == NULL) {
    fprintf (stderr, "Calling MIR_gen_set_debug_file before MIR_gen_init -- good bye\n");
    exit (1);
  }
  debug_file = f;
#endif
}

void MIR_gen_set_optimize_level (MIR_context_t ctx, unsigned int level) {
  struct gen_ctx *gen_ctx = *gen_ctx_loc (ctx);

  optimize_level = level;
}

void MIR_gen_init (MIR_context_t ctx) {
  struct gen_ctx **gen_ctx_ptr = gen_ctx_loc (ctx), *gen_ctx;
  MIR_reg_t i;

  *gen_ctx_ptr = gen_ctx = gen_malloc (ctx, sizeof (struct gen_ctx));
  optimize_level = 2;
  gen_ctx->target_ctx = NULL;
  gen_ctx->data_flow_ctx = NULL;
  gen_ctx->cse_ctx = NULL;
  gen_ctx->rdef_ctx = NULL;
  gen_ctx->rename_ctx = NULL;
  gen_ctx->licm_ctx = NULL;
  gen_ctx->ccp_ctx = NULL;
  gen_ctx->lr_ctx = NULL;
  gen_ctx->ra_ctx = NULL;
  gen_ctx->selection_ctx = NULL;
  #if !MIR_NO_GEN_DEBUG
    debug_file = NULL;
  #endif
  VARR_CREATE (loop_node_t, loop_nodes, 32);
  VARR_CREATE (loop_node_t, queue_nodes, 32);
  VARR_CREATE (loop_node_t, loop_entries, 16);
  init_dead_vars ();
  init_data_flow (ctx);
  init_cse (ctx);
  init_rdef (ctx);
  init_rename (ctx);
  init_licm (ctx);
  init_ccp (ctx);
  temp_bitmap = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  temp_bitmap2 = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  all_vars = bitmap_create2 (DEFAULT_INIT_BITMAP_BITS_NUM);
  init_live_ranges (ctx);
  init_ra (ctx);
  init_selection (ctx);
  target_init (ctx);
  call_used_hard_regs = bitmap_create2 (MAX_HARD_REG + 1);
  max_int_hard_regs = max_fp_hard_regs = 0;
  for (i = 0; i <= MAX_HARD_REG; i++) {
    if (target_fixed_hard_reg_p (i)) continue;
    if (target_call_used_hard_reg_p (i)) bitmap_set_bit_p (call_used_hard_regs, i);
    target_hard_reg_type_ok_p (i, MIR_T_I32) ? max_int_hard_regs++ : max_fp_hard_regs++;
  }
  insn_to_consider = bitmap_create2 (1024);
}

void MIR_gen_finish (MIR_context_t ctx) {
  struct gen_ctx **gen_ctx_ptr = gen_ctx_loc (ctx);
  struct gen_ctx *gen_ctx = *gen_ctx_ptr;

  finish_data_flow (ctx);
  finish_cse (ctx);
  finish_rdef (ctx);
  finish_rename (ctx);
  finish_licm (ctx);
  finish_ccp (ctx);
  bitmap_destroy (temp_bitmap);
  bitmap_destroy (temp_bitmap2);
  bitmap_destroy (all_vars);
  finish_live_ranges (ctx);
  finish_ra (ctx);
  finish_selection (ctx);
  bitmap_destroy (call_used_hard_regs);
  bitmap_destroy (insn_to_consider);
  target_finish (ctx);
  finish_dead_vars ();
  free (gen_ctx->data_flow_ctx);
  VARR_DESTROY (loop_node_t, loop_nodes);
  VARR_DESTROY (loop_node_t, queue_nodes);
  VARR_DESTROY (loop_node_t, loop_entries);
  free (gen_ctx);
  *gen_ctx_ptr = NULL;
}

void MIR_set_gen_interface (MIR_context_t ctx, MIR_item_t func_item) { MIR_gen (ctx, func_item); }

static void *gen_and_redirect (MIR_context_t ctx, MIR_item_t func_item) {
  MIR_gen (ctx, func_item);
  return func_item->u.func->machine_code;
}

void MIR_set_lazy_gen_interface (MIR_context_t ctx, MIR_item_t func_item) {
  void *addr = _MIR_get_wrapper (ctx, func_item, gen_and_redirect);

  _MIR_redirect_thunk (ctx, func_item->addr, addr);
}

/* Local Variables:                */
/* mode: c                         */
/* page-delimiter: "/\\* New Page" */
/* End:                            */
