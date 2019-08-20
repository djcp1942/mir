/* This file is a part of MIR project.
   Copyright (C) 2018, 2019 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include "mir.h"

DEF_VARR (MIR_insn_t);
DEF_VARR (MIR_reg_t);
DEF_VARR (MIR_op_t);
DEF_VARR (MIR_type_t);
DEF_HTAB (MIR_item_t);
DEF_VARR (MIR_module_t);
DEF_VARR (size_t);
DEF_VARR (char);
DEF_VARR (uint8_t);

struct string_context;
struct reg_context;
struct simplify_context;
struct machine_code_context;
struct io_context;
struct scan_context;
struct interp_context;

struct MIR_context {
  MIR_error_func_t error_func;
  VARR (MIR_insn_t) *temp_insns;
  VARR (MIR_op_t) *temp_insn_ops;
  VARR (MIR_var_t) *temp_vars;
  VARR (MIR_type_t) *temp_types;
  VARR (size_t) *insn_nops;
  VARR (char) *temp_string;
  VARR (uint8_t) *temp_data;
  char temp_buff[30];
  HTAB (MIR_item_t) *module_item_tab;
  /* Module to keep items potentially used by all modules:  */
  struct MIR_module environment_module;
  VARR (MIR_reg_t) *inline_reg_map;
  MIR_module_t curr_module;
  MIR_func_t curr_func;
  int curr_label_num;
  DLIST (MIR_module_t) all_modules;
  VARR (MIR_module_t) *modules_to_link;
  struct string_context *string_context;
  struct reg_context *reg_context;
  struct simplify_context *simplify_context;
  struct machine_code_context *machine_code_context;
  struct io_context *io_context;
  struct scan_context *scan_context;
  struct interp_context *interp_context;
};

#define error_func context->error_func
#define temp_insns context->temp_insns
#define temp_insn_ops context->temp_insn_ops
#define temp_vars context->temp_vars
#define temp_types context->temp_types
#define insn_nops context->insn_nops
#define temp_string context->temp_string
#define temp_data context->temp_data
#define temp_buff context->temp_buff
#define module_item_tab context->module_item_tab
#define environment_module context->environment_module
#define inline_reg_map context->inline_reg_map
#define curr_module context->curr_module
#define curr_func context->curr_func
#define curr_label_num context->curr_label_num
#define all_modules context->all_modules
#define modules_to_link context->modules_to_link

static void util_error (MIR_context_t context, const char *message);
#define MIR_VARR_ERROR util_error
#define MIR_HTAB_ERROR MIR_VARR_ERROR

#include "mir-hash.h"
#include "mir-htab.h"
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <float.h>
#include <ctype.h>
#include <limits.h>

static void interp_init (MIR_context_t context);
static void finish_func_interpretation (MIR_item_t func_item);
static void interp_finish (MIR_context_t context);

static void MIR_NO_RETURN default_error (enum MIR_error_type error_type, const char *format, ...) {
  va_list ap;

  va_start (ap, format);
  vfprintf (stderr, format, ap);
  fprintf (stderr, "\n");
  va_end (ap);
  exit (1);
}

static void MIR_NO_RETURN util_error (MIR_context_t context, const char *message) {
  (*error_func) (MIR_alloc_error, message);
}

#define TEMP_REG_NAME_PREFIX "t"
#define HARD_REG_NAME_PREFIX "hr"
#define TEMP_ITEM_NAME_PREFIX ".lc"

int _MIR_reserved_ref_name_p (MIR_context_t context, const char *name) {
  return strncmp (name, TEMP_ITEM_NAME_PREFIX, strlen (TEMP_ITEM_NAME_PREFIX)) == 0;
}

/* Reserved names:
   fp - frame pointer
   hr<number> - a hardware reg
   lc<number> - a temp item */
int _MIR_reserved_name_p (MIR_context_t context, const char *name) {
  size_t i, start;
  
  if (_MIR_reserved_ref_name_p (context, name))
    return TRUE;
  else if (strncmp (name, HARD_REG_NAME_PREFIX, strlen (HARD_REG_NAME_PREFIX)) == 0)
    start = strlen (HARD_REG_NAME_PREFIX);
  else
    return FALSE;
  for (i = start; name[i] != '\0'; i++)
    if (name[i] < '0' || name[i] > '9')
      return FALSE;
  return TRUE;
}

struct insn_desc {
  MIR_insn_code_t code; const char *name; unsigned op_modes[4];
};

#define OUTPUT_FLAG (1 << 31)

static const struct insn_desc insn_descs[] = {
  {MIR_MOV, "mov", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FMOV, "fmov", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DMOV, "dmov", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDMOV, "ldmov", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_EXT8, "ext8", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EXT16, "ext16", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EXT32, "ext32", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UEXT8, "uext8", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UEXT16, "uext16", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UEXT32, "uext32", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_I2F, "i2f", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_I2D, "i2d", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_I2LD, "i2ld", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UI2F, "ui2f", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UI2D, "ui2d", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UI2LD, "ui2ld", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_F2I, "f2i", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_D2I, "d2i", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LD2I, "ld2i", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_F2D, "f2d", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_F2LD, "f2ld", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_D2F, "d2f", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_D2LD, "d2ld", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LD2F, "ld2f", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_LD2D, "ld2d", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_NEG, "neg", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_NEGS, "negs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FNEG, "fneg", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DNEG, "dneg", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDNEG, "ldneg", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_ADD, "add", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ADDS, "adds", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FADD, "fadd", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DADD, "dadd", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDADD, "ldadd", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_SUB, "sub", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_SUBS, "subs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FSUB, "fsub", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DSUB, "dsub", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDSUB, "ldsub", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_MUL, "mul", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_MULS, "muls", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FMUL, "fmul", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DMUL, "dmul", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDMUL, "ldmul", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_DIV, "div", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_DIVS, "divs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UDIV, "udiv", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UDIVS, "udivs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FDIV, "fdiv", {MIR_OP_FLOAT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DDIV, "ddiv", {MIR_OP_DOUBLE | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDDIV, "lddiv", {MIR_OP_LDOUBLE | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_MOD, "mod", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_MODS, "mods", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UMOD, "umod", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UMODS, "umods", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_AND, "and", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ANDS, "ands", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_OR, "or", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ORS, "ors", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_XOR, "xor", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_XORS, "xors", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LSH, "lsh", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LSHS, "lshs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_RSH, "rsh", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_RSHS, "rshs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_URSH, "ursh", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_URSHS, "urshs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EQ, "eq", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_EQS, "eqs", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FEQ, "feq", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DEQ, "deq", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDEQ, "ldeq", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_NE, "ne", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_NES, "nes", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FNE, "fne", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DNE, "dne", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDNE, "ldne", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_LT, "lt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LTS, "lts", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULT, "ult", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULTS, "ults", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FLT, "flt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DLT, "dlt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDLT, "ldlt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_LE, "le", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LES, "les", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULE, "ule", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_ULES, "ules", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FLE, "fle", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DLE, "dle", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDLE, "ldle", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_GT, "gt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_GTS, "gts", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGT, "ugt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGTS, "ugts", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FGT, "fgt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DGT, "dgt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDGT, "ldgt", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_GE, "ge", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_GES, "ges", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGE, "uge", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UGES, "uges", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FGE, "fge", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DGE, "dge", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDGE, "ldge", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_JMP, "jmp", {MIR_OP_LABEL, MIR_OP_BOUND}},
  {MIR_BT, "bt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BTS, "bts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BF, "bf", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BFS, "bfs", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BEQ, "beq", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BEQS, "beqs", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBEQ, "fbeq", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBEQ, "dbeq", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBEQ, "ldbeq", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BNE, "bne", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BNES, "bnes", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBNE, "fbne", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBNE, "dbne", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBNE, "ldbne", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BLT, "blt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BLTS, "blts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLT, "ublt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLTS, "ublts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBLT, "fblt", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBLT, "dblt", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBLT, "ldblt", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BLE, "ble", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BLES, "bles", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLE, "uble", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBLES, "ubles", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBLE, "fble", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBLE, "dble", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBLE, "ldble", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BGT, "bgt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BGTS, "bgts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGT, "ubgt", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGTS, "ubgts", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBGT, "fbgt", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBGT, "dbgt", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBGT, "ldbgt", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_BGE, "bge", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BGES, "bges", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGE, "ubge", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_UBGES, "ubges", {MIR_OP_LABEL, MIR_OP_INT, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_FBGE, "fbge", {MIR_OP_LABEL, MIR_OP_FLOAT, MIR_OP_FLOAT, MIR_OP_BOUND}},
  {MIR_DBGE, "dbge", {MIR_OP_LABEL, MIR_OP_DOUBLE, MIR_OP_DOUBLE, MIR_OP_BOUND}},
  {MIR_LDBGE, "ldbge", {MIR_OP_LABEL, MIR_OP_LDOUBLE, MIR_OP_LDOUBLE, MIR_OP_BOUND}},
  {MIR_CALL, "call", {MIR_OP_BOUND}},
  {MIR_INLINE, "inline", {MIR_OP_BOUND}},
  {MIR_RET, "ret", {MIR_OP_BOUND}},
  {MIR_ALLOCA, "alloca", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_BSTART, "bstart", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_BOUND}},
  {MIR_BEND, "bend", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_VA_ARG, "va_arg", {MIR_OP_INT | OUTPUT_FLAG, MIR_OP_INT, MIR_OP_UNDEF, MIR_OP_BOUND}},
  {MIR_VA_START, "va_start", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_VA_END, "va_end", {MIR_OP_INT, MIR_OP_BOUND}},
  {MIR_LABEL, "label", {MIR_OP_BOUND}},
  {MIR_INVALID_INSN, "invalid-insn", {MIR_OP_BOUND}},
};

static void check_and_prepare_insn_descs (MIR_context_t context) {
  size_t i, j;
  
  VARR_CREATE (size_t, insn_nops, 0);
  for (i = 0; i < MIR_INSN_BOUND; i++) {
    mir_assert (insn_descs[i].code == i);
    for (j = 0; insn_descs[i].op_modes[j] != MIR_OP_BOUND; j++)
      ;
    VARR_PUSH (size_t, insn_nops, j);
  }
}

static MIR_op_mode_t type2mode (MIR_type_t type) {
  return (type == MIR_T_F ? MIR_OP_FLOAT : type == MIR_T_D ? MIR_OP_DOUBLE
	  : type == MIR_T_LD ? MIR_OP_LDOUBLE : MIR_OP_INT);
}



typedef struct string {
  size_t num; /* string number starting with 1 */
  const char *str;
} string_t;

DEF_VARR (string_t);
DEF_HTAB (string_t);

struct string_context {
  VARR (string_t) *strings;
  HTAB (string_t) *string_tab;
};

#define strings context->string_context->strings
#define string_tab context->string_context->string_tab

static htab_hash_t str_hash (string_t str) { return mir_hash (str.str, strlen (str.str), 0); }
static int str_eq (string_t str1, string_t str2) { return strcmp (str1.str, str2.str) == 0; }

static void string_init (VARR (string_t) **strs, HTAB (string_t) **str_tab) {
  string_t string = {0, NULL};
  
  VARR_CREATE (string_t, *strs, 0);
  VARR_PUSH (string_t, *strs, string); /* don't use 0th string */
  HTAB_CREATE (string_t, *str_tab, 1000, str_hash, str_eq);
}

static int string_find (VARR (string_t) **strs, HTAB (string_t) **str_tab,
			const char *str, string_t *s) {
  string_t string;
  
  string.str = str;
  return HTAB_DO (string_t, *str_tab, string, HTAB_FIND, *s);
}

static string_t string_store (MIR_context_t context, VARR (string_t) **strs,
			      HTAB (string_t) **str_tab, const char *str) {
  char *heap_str;
  string_t el, string;
  
  if (string_find (strs, str_tab, str, &el))
    return el;
  if ((heap_str = malloc (strlen (str) + 1)) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for strings");
  strcpy (heap_str, str);
  string.str = heap_str;
  string.num = VARR_LENGTH (string_t, *strs);
  VARR_PUSH (string_t, *strs, string);
  HTAB_DO (string_t, *str_tab, string, HTAB_INSERT, el);
  return string;
}

static void string_finish (VARR (string_t) **strs, HTAB (string_t) **str_tab) {
  size_t i;
  
  for (i = 1; i < VARR_LENGTH (string_t, *strs); i++)
    free ((char *) VARR_ADDR (string_t, *strs)[i].str);
  VARR_DESTROY (string_t, *strs);
  HTAB_DESTROY (string_t, *str_tab);
}



typedef struct reg_desc {
  size_t name_num;   /* 1st key for the namenum2rdn hash tab */
  MIR_func_t func;   /* 2nd key for hash the both tabs */
  MIR_type_t type;
  MIR_reg_t reg;     /* 1st key reg2rdn hash tab */
} reg_desc_t;

DEF_VARR (reg_desc_t);

typedef struct size_context {
  size_t rdn;
  MIR_context_t context;
} size_context_t;

DEF_HTAB (size_context_t);

struct reg_context {
  VARR (reg_desc_t) *reg_descs;
  HTAB (size_context_t) *namenum2rdn_tab;
  HTAB (size_context_t) *reg2rdn_tab;
};

#define reg_descs context->reg_context->reg_descs
#define namenum2rdn_tab context->reg_context->namenum2rdn_tab
#define reg2rdn_tab context->reg_context->reg2rdn_tab

static int namenum2rdn_eq (size_context_t sc1, size_context_t sc2) {
  MIR_context_t context = sc1.context;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, reg_descs);

  mir_assert (context == sc2.context);
  return addr[sc1.rdn].name_num == addr[sc2.rdn].name_num && addr[sc1.rdn].func == addr[sc2.rdn].func;
}

static htab_hash_t namenum2rdn_hash (size_context_t sc) {
  MIR_context_t context = sc.context;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, reg_descs);

  return mir_hash_finish (mir_hash_step
			  (mir_hash_step (mir_hash_init (0), (uint64_t) addr[sc.rdn].func),
			   (uint64_t) addr[sc.rdn].name_num));
}

static int reg2rdn_eq (size_context_t sc1, size_context_t sc2) {
  MIR_context_t context = sc1.context;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, reg_descs);
  
  mir_assert (context == sc2.context);
  return addr[sc1.rdn].reg == addr[sc2.rdn].reg && addr[sc1.rdn].func == addr[sc2.rdn].func;
}

static htab_hash_t reg2rdn_hash (size_context_t sc) {
  MIR_context_t context = sc.context;
  reg_desc_t *addr = VARR_ADDR (reg_desc_t, reg_descs);

  return mir_hash_finish (mir_hash_step
			  (mir_hash_step (mir_hash_init (0), (uint64_t) addr[sc.rdn].func),
			   addr[sc.rdn].reg));
}

static void reg_init (MIR_context_t context) {
  reg_desc_t rd = {0, NULL, MIR_T_I64, 0};
  
  if ((context->reg_context = malloc (sizeof (struct reg_context))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for context");
  VARR_CREATE (reg_desc_t, reg_descs, 300);
  VARR_PUSH (reg_desc_t, reg_descs, rd); /* for 0 reg */
  HTAB_CREATE (size_context_t, namenum2rdn_tab, 300, namenum2rdn_hash, namenum2rdn_eq);
  HTAB_CREATE (size_context_t, reg2rdn_tab, 300, reg2rdn_hash, reg2rdn_eq);
}

static int func_reg_p (MIR_context_t context, MIR_func_t func, const char *name) {
  size_context_t sc, tab_sc;
  reg_desc_t rd;
  int res;
  
  rd.name_num = string_store (context, &strings, &string_tab, name).num;
  rd.func = func;
  sc.rdn = VARR_LENGTH (reg_desc_t, reg_descs); sc.context = context;
  VARR_PUSH (reg_desc_t, reg_descs, rd);
  res = HTAB_DO (size_context_t, namenum2rdn_tab, sc, HTAB_FIND, tab_sc);
  VARR_POP (reg_desc_t, reg_descs);
  return res;
}

static MIR_reg_t create_func_reg (MIR_context_t context, MIR_func_t func, const char *name,
				  MIR_reg_t reg, MIR_type_t type, int any_p) {
  reg_desc_t rd;
  size_context_t sc, tab_sc;
  int htab_res;
  
  if (! any_p && _MIR_reserved_name_p (context, name))
    (*error_func) (MIR_reserved_name_error, "redefining a reserved name %s", name);
  rd.name_num = string_store (context, &strings, &string_tab, name).num;
  rd.func = func;
  rd.type = type;
  rd.reg = reg; /* 0 is reserved */
  sc.rdn = VARR_LENGTH (reg_desc_t, reg_descs); sc.context = context;
  VARR_PUSH (reg_desc_t, reg_descs, rd);
  if (HTAB_DO (size_context_t, namenum2rdn_tab, sc, HTAB_FIND, tab_sc)) {
    VARR_POP (reg_desc_t, reg_descs);
    (*error_func) (MIR_repeated_decl_error, "Repeated reg declaration %s", name);
  }
  htab_res = HTAB_DO (size_context_t, namenum2rdn_tab, sc, HTAB_INSERT, tab_sc);
  mir_assert (! htab_res);
  htab_res = HTAB_DO (size_context_t, reg2rdn_tab, sc, HTAB_INSERT, tab_sc);
  mir_assert (! htab_res);
  return reg;
}

static void reg_finish (MIR_context_t context) {
  VARR_DESTROY (reg_desc_t, reg_descs);
  HTAB_DESTROY (size_context_t, namenum2rdn_tab);
  HTAB_DESTROY (size_context_t, reg2rdn_tab);
  free (context->reg_context); context->reg_context = NULL;
}



static void push_data (MIR_context_t context, uint8_t *els, size_t size) {
  for (size_t i = 0; i < size; i++)
    VARR_PUSH (uint8_t, temp_data, els[i]);
}

const char *MIR_item_name (MIR_context_t context, MIR_item_t item) {
  return (item->item_type == MIR_func_item ? item->u.func->name
	  : item->item_type == MIR_proto_item ? item->u.proto->name
	  : item->item_type == MIR_import_item ? item->u.import
	  : item->item_type == MIR_export_item ? item->u.export
	  : item->item_type == MIR_forward_item ? item->u.forward
	  : item->item_type == MIR_bss_item ? item->u.bss->name
	  : item->item_type == MIR_data_item ? item->u.data->name
	  : item->item_type == MIR_ref_data_item ? item->u.ref_data->name
	  : item->u.expr_data->name);
}

#if MIR_IO
static void io_init (MIR_context_t context);
static void io_finish (MIR_context_t context);
#endif

#if MIR_SCAN
static void scan_init (MIR_context_t context);
static void scan_finish (MIR_context_t context);
#endif

static void vn_init (MIR_context_t context);
static void vn_finish (MIR_context_t context);

MIR_error_func_t MIR_get_error_func (MIR_context_t context) { return error_func; }

void MIR_set_error_func (MIR_context_t context, MIR_error_func_t func) { error_func = func; }

static htab_hash_t item_hash (MIR_item_t it) {
  return mir_hash_finish (mir_hash_step
			  (mir_hash_step (mir_hash_init (28),
					  (uint64_t) MIR_item_name (NULL, it)),
			   (uint64_t) it->module));
}
static int item_eq (MIR_item_t it1, MIR_item_t it2) {
  return it1->module == it2->module && MIR_item_name (NULL, it1) == MIR_item_name (NULL, it2);
}

static MIR_item_t find_item (MIR_context_t context, const char *name, MIR_module_t module) {
  MIR_item_t tab_item;
  struct MIR_item item_s;
  struct MIR_func func_s;
  
  item_s.item_type = MIR_func_item;
  func_s.name = name;
  item_s.module = module;
  item_s.u.func = &func_s;
  if (HTAB_DO (MIR_item_t, module_item_tab, &item_s, HTAB_FIND, tab_item))
    return tab_item;
  return NULL;
}

static void init_module (MIR_context_t context, MIR_module_t m, const char *name) {
  m->data = NULL;
  m->temp_items_num = 0;
  m->name = string_store (context, &strings, &string_tab, name).str;
  DLIST_INIT (MIR_item_t, m->items);
}

static void code_init (MIR_context_t context);
static void code_finish (MIR_context_t context);

MIR_context_t MIR_init (void) {
  MIR_context_t context;
  
  if ((context = malloc (sizeof (struct MIR_context))) == NULL)
    default_error (MIR_alloc_error, "Not enough memory for context");
  context->string_context = NULL; context->reg_context = NULL; context->simplify_context = NULL;
  context->machine_code_context = NULL; context->io_context = NULL; context->scan_context = NULL;
  context->interp_context = NULL;
#ifndef NDEBUG
  for (MIR_insn_code_t c = 0; c < MIR_INVALID_INSN; c++)
    mir_assert (c == insn_descs[c].code);
#endif
  error_func = default_error;
  curr_module = NULL;
  curr_func = NULL;
  curr_label_num = 0;
  if ((context->string_context = malloc (sizeof (struct string_context))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for context");
  string_init (&strings, &string_tab);
  reg_init (context);
  VARR_CREATE (MIR_insn_t, temp_insns, 0);
  VARR_CREATE (MIR_op_t, temp_insn_ops, 0);
  VARR_CREATE (MIR_var_t, temp_vars, 0);
  VARR_CREATE (MIR_type_t, temp_types, 0);
  check_and_prepare_insn_descs (context);
  DLIST_INIT (MIR_module_t, all_modules);
  vn_init (context);
  VARR_CREATE (MIR_reg_t, inline_reg_map, 256);
  VARR_CREATE (char, temp_string, 64);
  VARR_CREATE (uint8_t, temp_data, 512);
#if MIR_IO
  io_init (context);
#endif
#if MIR_SCAN
  scan_init (context);
#endif
  VARR_CREATE (MIR_module_t, modules_to_link, 0);
  init_module (context, &environment_module, ".environment");
  HTAB_CREATE (MIR_item_t, module_item_tab, 512, item_hash, item_eq);
  code_init (context);
  interp_init (context);
  return context;
}

void MIR_finish (MIR_context_t context) {
  interp_finish (context);
  HTAB_DESTROY (MIR_item_t, module_item_tab);
  VARR_DESTROY (MIR_module_t, modules_to_link);
#if MIR_SCAN
  scan_finish (context);
#endif
#if MIR_IO
  io_finish (context);
#endif
  VARR_DESTROY (uint8_t, temp_data);
  VARR_DESTROY (char, temp_string);
  VARR_DESTROY (MIR_reg_t, inline_reg_map);
  reg_finish (context);
  string_finish (&strings, &string_tab);
  vn_finish (context);
  VARR_DESTROY (MIR_var_t, temp_vars);
  VARR_DESTROY (size_t, insn_nops);
  VARR_DESTROY (MIR_op_t, temp_insn_ops);
  VARR_DESTROY (MIR_insn_t, temp_insns);
  VARR_DESTROY (MIR_type_t, temp_types);
  code_finish (context);
  if (curr_func != NULL)
    (*error_func) (MIR_finish_error, "finish when function %s is not finished", curr_func->name);
  if (curr_module != NULL)
    (*error_func) (MIR_finish_error, "finish when module %s is not finished", curr_module->name);
  free (context->string_context);
  free (context); context = NULL;
}

MIR_module_t MIR_new_module (MIR_context_t context, const char *name) {
  if (curr_module != NULL)
    (*error_func) (MIR_nested_module_error,
		   "Creating module when previous module %s is not finished", curr_module->name);
  if ((curr_module = malloc (sizeof (struct MIR_module))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for module %s creation", name);
  init_module (context, curr_module, name);
  DLIST_APPEND (MIR_module_t, all_modules, curr_module);
  return curr_module;
}

DLIST (MIR_module_t) *MIR_get_module_list (MIR_context_t context) {
  return &all_modules;
}

static const char *type_str (MIR_type_t tp) {
  switch (tp) {
  case MIR_T_I8: return "i8";
  case MIR_T_U8: return "u8";
  case MIR_T_I16: return "i16";
  case MIR_T_U16: return "u16";
  case MIR_T_I32: return "i32";
  case MIR_T_U32: return "u32";
  case MIR_T_I64: return "i64";
  case MIR_T_U64: return "u64";
  case MIR_T_F: return "f";
  case MIR_T_D: return "d";
  case MIR_T_LD: return "ld";
  case MIR_T_P: return "p";
  default: return "";
  }
}

const char *MIR_type_str (MIR_context_t context, MIR_type_t tp) {
  const char *str = type_str (tp);
  
  if (strcmp (str, "") == 0)
    (*error_func) (MIR_wrong_param_value_error, "MIR_type_str: wrong type");
  return str;
}

static MIR_item_t add_item (MIR_context_t context, MIR_item_t item) {
  int replace_p;
  MIR_item_t tab_item;
  
  if ((tab_item = find_item (context, MIR_item_name (context, item), item->module)) == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
    HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, item);
    return item;
  }
  switch (tab_item->item_type) {
  case MIR_import_item:
    if (item->item_type != MIR_import_item)
      (*error_func) (MIR_import_export_error, "existing module definition %s already defined as import",
		     tab_item->u.import);
    item = tab_item;
    break;
  case MIR_export_item:
  case MIR_forward_item:
    replace_p = FALSE;
    if (item->item_type == MIR_import_item) {
      (*error_func) (MIR_import_export_error, "export/forward of import %s", item->u.import);
    } else if (item->item_type != MIR_export_item && item->item_type != MIR_forward_item) {
      replace_p = TRUE;
      DLIST_APPEND (MIR_item_t, curr_module->items, item);
    } else {
      if (tab_item->item_type == item->item_type)
	item = tab_item;
      else
	DLIST_APPEND (MIR_item_t, curr_module->items, item);
      if (item->item_type == MIR_export_item && tab_item->item_type == MIR_forward_item)
	replace_p = TRUE;
    }
    if (replace_p) { /* replace forward by export or export/forward by its definition: */
      tab_item->ref_def = item;
      if (tab_item->item_type == MIR_export_item)
	item->export_p = TRUE;
      HTAB_DO (MIR_item_t, module_item_tab, tab_item, HTAB_DELETE, tab_item);
      HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, tab_item);
      mir_assert (item == tab_item);
    }
    break;
  case MIR_proto_item:
    (*error_func) (MIR_repeated_decl_error, "item %s was already defined as proto", tab_item->u.proto->name);
    break;
  case MIR_bss_item:
  case MIR_data_item:
  case MIR_ref_data_item:
  case MIR_expr_data_item:
  case MIR_func_item:
    if (item->item_type == MIR_export_item) {
      if (tab_item->export_p) {
	item = tab_item;
      } else { /* just keep one export: */
	tab_item->export_p = TRUE;
	DLIST_APPEND (MIR_item_t, curr_module->items, item);
	item->ref_def = tab_item;
      }
    } else if (item->item_type == MIR_forward_item) {
      DLIST_APPEND (MIR_item_t, curr_module->items, item);
      item->ref_def = tab_item;
    } else if (item->item_type == MIR_import_item) {
      (*error_func) (MIR_import_export_error, "import of local definition %s", item->u.import);
    } else {
      (*error_func) (MIR_repeated_decl_error, "Repeated item declaration %s", MIR_item_name (context, item));
    }
    break;
  default:
    mir_assert (FALSE);
  }
  return item;
}

static MIR_item_t create_item (MIR_context_t context,
			       MIR_item_type_t item_type, const char *item_name) {
  MIR_item_t item;
  
  if (curr_module == NULL)
    (*error_func) (MIR_no_module_error, "%s outside module", item_name);
  if ((item = malloc (sizeof (struct MIR_item))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for creation of item %s", item_name);
  item->data = NULL;
  item->module = curr_module;
  item->item_type = item_type;
  item->ref_def = NULL;
  item->export_p = FALSE;
  item->addr = item->machine_code = NULL;
  return item;
}

static MIR_item_t new_export_import_forward (MIR_context_t context, const char *name,
					     MIR_item_type_t item_type, const char *item_name,
					     int create_only_p) {
  MIR_item_t item, tab_item;
  const char *uniq_name;
    
  item = create_item (context, item_type, item_name);
  uniq_name = string_store (context, &strings, &string_tab, name).str;
  if (item_type == MIR_export_item)
    item->u.export = uniq_name;
  else if (item_type == MIR_import_item)
    item->u.import = uniq_name;
  else
    item->u.forward = uniq_name;
  if (create_only_p)
    return item;
  if ((tab_item = add_item (context, item)) != item) {
    free (item);
    item = tab_item;
  }
  return item;
}

MIR_item_t MIR_new_export (MIR_context_t context, const char *name) {
  return new_export_import_forward (context, name, MIR_export_item, "export", FALSE);
}

MIR_item_t MIR_new_import (MIR_context_t context, const char *name) {
  return new_export_import_forward (context, name, MIR_import_item, "import", FALSE);
}

MIR_item_t MIR_new_forward (MIR_context_t context, const char *name) {
  return new_export_import_forward (context, name, MIR_forward_item, "forward", FALSE);
}

MIR_item_t MIR_new_bss (MIR_context_t context, const char *name, size_t len) {
  MIR_item_t tab_item, item = create_item (context, MIR_bss_item, "bss");
  
  item->u.bss = malloc (sizeof (struct MIR_bss));
  if (item->u.bss == NULL) {
    free (item);
    (*error_func) (MIR_alloc_error, "Not enough memory for creation of bss %s", name);
  }
  if (name != NULL)
    name = string_store (context, &strings, &string_tab, name).str;
  item->u.bss->name = name;
  item->u.bss->len = len;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if (add_item (context, item) != item) {
    free (item);
    item = tab_item;
  }
  return item;
}

size_t _MIR_type_size (MIR_context_t context, MIR_type_t type) {
  switch (type) {
  case MIR_T_I8: return sizeof (int8_t);
  case MIR_T_U8: return sizeof (uint8_t);
  case MIR_T_I16: return sizeof (int16_t);
  case MIR_T_U16: return sizeof (uint16_t);
  case MIR_T_I32: return sizeof (int32_t);
  case MIR_T_U32: return sizeof (uint32_t);
  case MIR_T_I64: return sizeof (int64_t);
  case MIR_T_U64: return sizeof (uint64_t);
  case MIR_T_F: return sizeof (float);
  case MIR_T_D: return sizeof (double);
  case MIR_T_LD: return sizeof (long double);
  case MIR_T_P: return sizeof (void *);
  default:
    mir_assert (FALSE);
    return 1;
  }
}

MIR_item_t MIR_new_data (MIR_context_t context, const char *name,
			 MIR_type_t el_type, size_t nel, const void *els) {
  MIR_item_t tab_item, item = create_item (context, MIR_data_item, "data");
  MIR_data_t data;
  size_t el_len = _MIR_type_size (context, el_type);
  
  item->u.data = data = malloc (sizeof (struct MIR_data) + el_len * nel);
  if (data == NULL) {
    free (item);
    (*error_func) (MIR_alloc_error, "Not enough memory for creation of data %s", name == NULL ? "" : name);
  }
  if (name != NULL)
    name = string_store (context, &strings, &string_tab, name).str;
  data->name = name;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if (add_item (context, item) != item) {
    free (item);
    item = tab_item;
  }
  data->el_type = el_type;
  data->nel = nel;
  memcpy (data->u.els, els, el_len * nel);
  return item;
}

MIR_item_t MIR_new_string_data (MIR_context_t context, const char *name, const char *str) {
  return MIR_new_data (context, name, MIR_T_U8, strlen (str) + 1, str);
}

MIR_item_t MIR_new_ref_data (MIR_context_t context, const char *name, MIR_item_t ref_item) {
  MIR_item_t tab_item, item = create_item (context, MIR_ref_data_item, "ref data");
  MIR_ref_data_t ref_data;
  
  item->u.ref_data = ref_data = malloc (sizeof (struct MIR_ref_data));
  if (ref_data == NULL) {
    free (item);
    (*error_func) (MIR_alloc_error,
		   "Not enough memory for creation of ref data %s", name == NULL ? "" : name);
  }
  if (name != NULL)
    name = string_store (context, &strings, &string_tab, name).str;
  ref_data->name = name;
  ref_data->ref_item = ref_item;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if (add_item (context, item) != item) {
    free (item);
    item = tab_item;
  }
  return item;
}

MIR_item_t MIR_new_expr_data (MIR_context_t context, const char *name, MIR_item_t expr_item) {
  MIR_item_t tab_item, item = create_item (context, MIR_expr_data_item, "expr data");
  MIR_expr_data_t expr_data;
  
  item->u.expr_data = expr_data = malloc (sizeof (struct MIR_expr_data));
  if (expr_data == NULL) {
    free (item);
    (*error_func) (MIR_alloc_error,
		   "Not enough memory for creation of expr data %s", name == NULL ? "" : name);
  }
  if (expr_item->item_type != MIR_func_item || expr_item->u.func->vararg_p
      || expr_item->u.func->nargs != 0 || expr_item->u.func->nres != 1)
    (*error_func) (MIR_binary_io_error,
		   "%s can not be an expr which should be non-argument, one result function",
		   MIR_item_name (context, expr_item));
  if (name != NULL)
    name = string_store (context, &strings, &string_tab, name).str;
  expr_data->name = name;
  expr_data->expr_item = expr_item;
  if (name == NULL) {
    DLIST_APPEND (MIR_item_t, curr_module->items, item);
  } else if (add_item (context, item) != item) {
    free (item);
    item = tab_item;
  }
  return item;
}

static MIR_item_t new_proto_arr (MIR_context_t context, const char *name,
				 size_t nres, MIR_type_t *res_types,
				 size_t nargs, int vararg_p, MIR_var_t *args) {
  MIR_item_t proto_item, tab_item;
  MIR_proto_t proto;
  size_t i;
  
  if (curr_module == NULL)
    (*error_func) (MIR_no_module_error, "Creating proto %s outside module", name);
  proto_item = create_item (context, MIR_proto_item, "proto");
  proto_item->u.proto = proto = malloc (sizeof (struct MIR_proto) + nres * sizeof (MIR_type_t));
  if (proto == NULL) {
    free (proto_item);
    (*error_func) (MIR_alloc_error, "Not enough memory for creation of proto %s", name);
  }
  proto->name = string_store (context, &strings, &string_tab, name).str;
  proto->res_types = (MIR_type_t *) ((char *) proto + sizeof (struct MIR_proto));
  memcpy (proto->res_types, res_types, nres * sizeof (MIR_type_t));
  proto->nres = nres;
  proto->vararg_p = vararg_p != 0;
  tab_item = add_item (context, proto_item);
  mir_assert (tab_item == proto_item);
  VARR_CREATE (MIR_var_t, proto->args, nargs);
  for (i = 0; i < nargs; i++)
    VARR_PUSH (MIR_var_t, proto->args, args[i]);
  return proto_item;
}

MIR_item_t MIR_new_proto_arr (MIR_context_t context, const char *name,
			      size_t nres, MIR_type_t *res_types,
			      size_t nargs, MIR_var_t *args) {
  return new_proto_arr (context, name, nres, res_types, nargs, FALSE, args);
}

MIR_item_t MIR_new_vararg_proto_arr (MIR_context_t context, const char *name,
				     size_t nres, MIR_type_t *res_types,
				     size_t nargs, MIR_var_t *args) {
  return new_proto_arr (context, name, nres, res_types, nargs, TRUE, args);
}

static MIR_item_t new_proto (MIR_context_t context, const char *name,
			     size_t nres, MIR_type_t *res_types,
			     size_t nargs, int vararg_p, va_list argp) {
  MIR_var_t arg;
  size_t i;
  
  VARR_TRUNC (MIR_var_t, temp_vars, 0);
  for (i = 0; i < nargs; i++) {
    arg.type = va_arg (argp, MIR_type_t);
    arg.name = va_arg (argp, const char *);
    VARR_PUSH (MIR_var_t, temp_vars, arg);
  }
  return new_proto_arr (context, name, nres, res_types, nargs,
			vararg_p, VARR_ADDR (MIR_var_t, temp_vars));
}

MIR_item_t MIR_new_proto (MIR_context_t context, const char *name,
			  size_t nres, MIR_type_t *res_types, size_t nargs, ...) {
  va_list argp;
  MIR_item_t proto_item;
  
  va_start (argp, nargs);
  proto_item = new_proto (context, name, nres, res_types, nargs, FALSE, argp);
  va_end(argp);
  return proto_item;
}

MIR_item_t MIR_new_vararg_proto (MIR_context_t context, const char *name,
				 size_t nres, MIR_type_t *res_types, size_t nargs, ...) {
  va_list argp;
  MIR_item_t proto_item;
  
  va_start (argp, nargs);
  proto_item = new_proto (context, name, nres, res_types, nargs, TRUE, argp);
  va_end(argp);
  return proto_item;
}

static MIR_item_t new_func_arr (MIR_context_t context, const char *name,
				size_t nres, MIR_type_t *res_types,
				size_t nargs, int vararg_p, MIR_var_t *vars) {
  MIR_item_t func_item, tab_item;
  MIR_func_t func;
  
  if (curr_func != NULL)
    (*error_func) (MIR_nested_func_error, "Creating function when previous function %s is not finished",
		   curr_func->name);
  if (nargs == 0 && vararg_p)
    (*error_func) (MIR_vararg_func_error, "Variable arg function %s w/o any mandatory argument", name);
  func_item = create_item (context, MIR_func_item, "function");
  curr_func = func_item->u.func = func = malloc (sizeof (struct MIR_func) + nres * sizeof (MIR_type_t));
  if (func == NULL) {
    free (func_item);
    (*error_func) (MIR_alloc_error, "Not enough memory for creation of func %s", name);
  }
  func->name = string_store (context, &strings, &string_tab, name).str;
  func->nres = nres;
  func->res_types = (MIR_type_t *) ((char *) func + sizeof (struct MIR_func));
  memcpy (func->res_types, res_types, nres * sizeof (MIR_type_t));
  tab_item = add_item (context, func_item);
  mir_assert (tab_item == func_item);
  DLIST_INIT (MIR_insn_t, func->insns);
  VARR_CREATE (MIR_var_t, func->vars, nargs + 8);
  func->nargs = nargs; func->last_temp_num = 0;
  func->vararg_p = vararg_p != 0;
  func->expr_p = FALSE;
  func->n_inlines = 0;
  for (size_t i = 0; i < nargs; i++) {
    MIR_type_t type = vars[i].type;
    
    VARR_PUSH (MIR_var_t, func->vars, vars[i]);
    create_func_reg (context, func, vars[i].name, i + 1,
		     type == MIR_T_F || type == MIR_T_D || type == MIR_T_LD ? type : MIR_T_I64, FALSE);
  }
  return func_item;
}

MIR_item_t MIR_new_func_arr (MIR_context_t context, const char *name,
			     size_t nres, MIR_type_t *res_types,
			     size_t nargs, MIR_var_t *vars) {
  return new_func_arr (context, name, nres, res_types, nargs, FALSE, vars);
}

MIR_item_t MIR_new_vararg_func_arr (MIR_context_t context, const char *name,
				    size_t nres, MIR_type_t *res_types,
				    size_t nargs, MIR_var_t *vars) {
  return new_func_arr (context, name, nres, res_types, nargs, TRUE, vars);
}

static MIR_item_t new_func (MIR_context_t context, const char *name,
			    size_t nres, MIR_type_t *res_types,
			    size_t nargs, int vararg_p, va_list argp) {
  MIR_var_t var;
  size_t i;
  
  VARR_TRUNC (MIR_var_t, temp_vars, 0);
  for (i = 0; i < nargs; i++) {
    var.type = va_arg (argp, MIR_type_t);
    var.name = va_arg (argp, const char *);
    VARR_PUSH (MIR_var_t, temp_vars, var);
  }
  return new_func_arr (context, name, nres, res_types, nargs,
		       vararg_p, VARR_ADDR (MIR_var_t, temp_vars));
}

MIR_item_t MIR_new_func (MIR_context_t context, const char *name,
			 size_t nres, MIR_type_t *res_types, size_t nargs, ...) {
  va_list argp;
  MIR_item_t func_item;
  
  va_start (argp, nargs);
  func_item = new_func (context, name, nres, res_types, nargs, FALSE, argp);
  va_end (argp);
  return func_item;
}

MIR_item_t MIR_new_vararg_func (MIR_context_t context, const char *name,
				size_t nres, MIR_type_t *res_types, size_t nargs, ...) {
  va_list argp;
  MIR_item_t func_item;
  
  va_start (argp, nargs);
  func_item = new_func (context, name, nres, res_types, nargs, TRUE, argp);
  va_end (argp);
  return func_item;
}

MIR_reg_t MIR_new_func_reg (MIR_context_t context, MIR_func_t func,
			    MIR_type_t type, const char *name) {
  MIR_var_t var;
  
  if (type != MIR_T_I64 && type != MIR_T_F && type != MIR_T_D && type != MIR_T_LD)
    (*error_func) (MIR_reg_type_error, "wrong type for register %s", name);
  var.type = type; var.name = string_store (context, &strings, &string_tab, name).str;
  VARR_PUSH (MIR_var_t, func->vars, var);
  return create_func_reg (context, func, name, VARR_LENGTH (MIR_var_t, func->vars), type, FALSE);
}

static reg_desc_t *find_rd_by_name_num (MIR_context_t context, size_t name_num, MIR_func_t func) {
  size_context_t sc, temp_sc;
  reg_desc_t rd;

  rd.name_num = name_num; rd.func = func; /* keys */
  rd.type = MIR_T_I64; rd.reg = 0; /* to eliminate warnings */
  temp_sc.rdn = VARR_LENGTH (reg_desc_t, reg_descs); temp_sc.context = context;
  VARR_PUSH (reg_desc_t, reg_descs, rd);
  if (! HTAB_DO (size_context_t, namenum2rdn_tab, temp_sc, HTAB_FIND, sc)) {
    VARR_POP (reg_desc_t, reg_descs);
    return NULL; /* undeclared */
  }
  VARR_POP (reg_desc_t, reg_descs);
  return &VARR_ADDR (reg_desc_t, reg_descs)[sc.rdn];
}

static reg_desc_t *find_rd_by_reg (MIR_context_t context, MIR_reg_t reg, MIR_func_t func) {
  size_context_t sc, temp_sc;
  reg_desc_t rd;

  rd.reg = reg; rd.func = func; /* keys */
  rd.name_num = 0; rd.type = MIR_T_I64; /* to eliminate warnings */
  temp_sc.rdn = VARR_LENGTH (reg_desc_t, reg_descs); temp_sc.context = context;
  VARR_PUSH (reg_desc_t, reg_descs, rd);
  if (! HTAB_DO (size_context_t, reg2rdn_tab, temp_sc, HTAB_FIND, sc)) {
    VARR_POP (reg_desc_t, reg_descs);
    (*error_func) (MIR_undeclared_func_reg_error, "undeclared reg %u of func %s", reg, func->name);
  }
  VARR_POP (reg_desc_t, reg_descs);
  return &VARR_ADDR (reg_desc_t, reg_descs)[sc.rdn];
}

void MIR_finish_func (MIR_context_t context) {
  int expr_p = TRUE;
  MIR_insn_t insn;
  MIR_insn_code_t code;
  
  if (curr_func == NULL)
    (*error_func) (MIR_no_func_error, "finish of non-existing function");
  if (curr_func->vararg_p || curr_func->nargs != 0 || curr_func->nres != 1)
    expr_p = FALSE;
  for (insn = DLIST_HEAD (MIR_insn_t, curr_func->insns);
       insn != NULL;
       insn = DLIST_NEXT (MIR_insn_t, insn)) {
    size_t i, actual_nops = MIR_insn_nops (context, insn);
    MIR_op_mode_t mode, expected_mode;
    reg_desc_t *rd;
    int out_p, can_be_out_p;
    
    code = insn->code;
    if (!curr_func->vararg_p && (code == MIR_VA_START || code == MIR_VA_END || code == MIR_VA_ARG)) {
      curr_func = NULL;
      (*error_func) (MIR_vararg_func_error,
		     "va_start, va_end, or va_arg are not in vararg function");
    } else if (code == MIR_RET && actual_nops != curr_func->nres) {
      curr_func = NULL;
      (*error_func) (MIR_vararg_func_error,
		     "number of operands in return does not correspond number of function returns");
    } else if (MIR_call_code_p (code))
      expr_p = FALSE;
    for (i = 0; i < actual_nops; i++) {
      if (MIR_call_code_p (code)) {
	if (i == 0) {
	  mir_assert (insn->ops[i].mode == MIR_OP_REF
		      && insn->ops[i].u.ref->item_type == MIR_proto_item);
	  continue; /* We checked the operand during insn creation -- skip the prototype */
	} else if (i == 1 && insn->ops[i].mode == MIR_OP_REF) {
	  mir_assert (insn->ops[i].u.ref->item_type == MIR_import_item
		      || insn->ops[i].u.ref->item_type == MIR_export_item
		      || insn->ops[i].u.ref->item_type == MIR_forward_item
		      || insn->ops[i].u.ref->item_type == MIR_func_item);
	  continue; /* We checked the operand during insn creation -- skip the func */
	}
      }
      if (code == MIR_VA_ARG && i == 2) {
	mir_assert (insn->ops[i].mode == MIR_OP_MEM);
	continue; /* We checked the operand during insn creation -- skip va_arg type  */
      }
      if (code != MIR_RET) {
	expected_mode = MIR_insn_op_mode (context, insn, i, &out_p);
      } else {
	out_p = FALSE;
	expected_mode = (curr_func->res_types[i] == MIR_T_F ? MIR_OP_FLOAT
			 : curr_func->res_types[i] == MIR_T_D ? MIR_OP_DOUBLE
			 : curr_func->res_types[i] == MIR_T_LD ? MIR_OP_LDOUBLE
			 : MIR_OP_INT);
      }
      can_be_out_p = TRUE;
      switch (insn->ops[i].mode) {
      case MIR_OP_REG:
	rd = find_rd_by_reg (context, insn->ops[i].u.reg, curr_func);
	mir_assert (rd != NULL && insn->ops[i].u.reg == rd->reg);
	mode = type2mode (rd->type);
	break;
      case MIR_OP_MEM:
	expr_p = FALSE;
	if (insn->ops[i].u.mem.base != 0) {
	  rd = find_rd_by_reg (context, insn->ops[i].u.mem.base, curr_func);
	  mir_assert (rd != NULL && insn->ops[i].u.mem.base == rd->reg);
	  if (type2mode (rd->type) != MIR_OP_INT) {
	    curr_func = NULL;
	    (*error_func) (MIR_reg_type_error, "base reg of non-integer type");
	  }
	}
	if (insn->ops[i].u.mem.index != 0) {
	  rd = find_rd_by_reg (context, insn->ops[i].u.mem.index, curr_func);
	  mir_assert (rd != NULL && insn->ops[i].u.mem.index == rd->reg);
	  if (type2mode (rd->type) != MIR_OP_INT) {
	    curr_func = NULL;
	    (*error_func) (MIR_reg_type_error, "index reg of non-integer type");
	  }
	}
	mode = type2mode (insn->ops[i].u.mem.type);
	break;
      case MIR_OP_HARD_REG:
      case MIR_OP_HARD_REG_MEM:
	expr_p = FALSE;
	mode = expected_mode;
	mir_assert (FALSE); /* Should not be here */
	break;
      default:
	can_be_out_p = FALSE;
	mode = insn->ops[i].mode;
	if (mode == MIR_OP_REF || mode == MIR_OP_STR)
	  mode = MIR_OP_INT; /* just an address */
	break;
      }
      insn->ops[i].value_mode = mode;
      if (expected_mode != MIR_OP_UNDEF
	  && (mode == MIR_OP_UINT ? MIR_OP_INT : mode) != expected_mode) {
	curr_func = NULL;
	(*error_func) (MIR_op_mode_error, "unexpected operand mode");
      }
      if (out_p && ! can_be_out_p) {
	curr_func = NULL;
	(*error_func) (MIR_out_op_error, "wrong operand for insn output");
      }
    }
  }
  curr_func->expr_p = expr_p;
  curr_func = NULL;
}

void MIR_finish_module (MIR_context_t context) {
  const char *name;
  MIR_item_t found_item;
  int global_p, new_p;
  
  if (curr_module == NULL)
    (*error_func) (MIR_no_module_error, "finish of non-existing module");
  curr_module = NULL;
}

static void setup_global (MIR_context_t context, const char *name, void *addr, MIR_item_t def) {
  MIR_item_t item, tab_item;
  MIR_module_t saved = curr_module;

  curr_module = &environment_module;
  /* Use import for proto representation: */
  item = new_export_import_forward (context, name, MIR_import_item, "import", TRUE);
  if ((tab_item = find_item (context, MIR_item_name (context, item), &environment_module))
      != item && tab_item != NULL) {
    free (item);
  } else {
    HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, tab_item);
    DLIST_APPEND (MIR_item_t, environment_module.items, item);
    tab_item = item;
  }
  tab_item->addr = addr;
  tab_item->ref_def = def;
  curr_module = saved;
}

static void undefined_interface (MIR_context_t context) {
  (*error_func) (MIR_call_op_error, "undefined call interface");
}

static MIR_item_t load_bss_data_section (MIR_context_t context, MIR_item_t item, int first_only_p) {
  const char *name;
  MIR_item_t curr_item, last_item, expr_item;
  size_t len, section_size = 0;
  uint8_t *addr;
  
  if (item->addr == NULL) {
    /* Calculate section size: */
    for (curr_item = item;
	 curr_item != NULL && curr_item->addr == NULL;
	 curr_item = first_only_p ? NULL : DLIST_NEXT (MIR_item_t, curr_item))
      if (curr_item->item_type == MIR_bss_item
	  && (curr_item == item || curr_item->u.bss->name == NULL))
	section_size += curr_item->u.bss->len;
      else if (curr_item->item_type == MIR_data_item
	       && (curr_item == item || curr_item->u.data->name == NULL))
	section_size += (curr_item->u.data->nel
			 * _MIR_type_size (context, curr_item->u.data->el_type));
      else if (curr_item->item_type == MIR_ref_data_item
	       && (curr_item == item || curr_item->u.ref_data->name == NULL))
	section_size += _MIR_type_size (context, MIR_T_P);
      else if (curr_item->item_type == MIR_expr_data_item
	       && (curr_item == item || curr_item->u.expr_data->name == NULL)) {
	expr_item = curr_item->u.expr_data->expr_item;
	if (expr_item->item_type != MIR_func_item
	    || ! expr_item->u.func->expr_p || expr_item->u.func->nres != 1)
	  (*error_func) (MIR_binary_io_error,
			 "%s can not be an expr which should be a func w/o calls and memory ops",
			 MIR_item_name (context, expr_item));
	section_size += _MIR_type_size (context, expr_item->u.func->res_types[0]);
      } else
	break;
    if ((item->addr = malloc (section_size)) == NULL) {
      name = MIR_item_name (context, item);
      (*error_func) (MIR_alloc_error, "Not enough memory to allocate data/bss %s",
		     name == NULL ? "" : name);
    }
  }
  /* Set up section memory: */
  for (last_item = item, curr_item = item, addr = item->addr;
       curr_item != NULL && (curr_item == item || curr_item->addr == NULL);
       last_item = curr_item, curr_item = first_only_p ? NULL : DLIST_NEXT (MIR_item_t, curr_item))
    if (curr_item->item_type == MIR_bss_item
	&& (curr_item == item || curr_item->u.bss->name == NULL)) {
      memset (addr, 0, curr_item->u.bss->len);
      curr_item->addr = addr;
      addr += curr_item->u.bss->len;
    } else if (curr_item->item_type == MIR_data_item
	       && (curr_item == item || curr_item->u.data->name == NULL)) {
      len = curr_item->u.data->nel * _MIR_type_size (context, curr_item->u.data->el_type);
      memmove (addr, curr_item->u.data->u.els, len);
      curr_item->addr = addr;
      addr += len;
    } else if (curr_item->item_type == MIR_ref_data_item
	       && (curr_item == item || curr_item->u.ref_data->name == NULL)) {
      curr_item->u.ref_data->load_addr = addr;
      curr_item->addr = addr;
      addr += _MIR_type_size (context, MIR_T_P);
    } else if (curr_item->item_type == MIR_expr_data_item
	       && (curr_item == item || curr_item->u.expr_data->name == NULL)) {
      expr_item = curr_item->u.expr_data->expr_item;
      len = _MIR_type_size (context, expr_item->u.func->res_types[0]);
      curr_item->u.expr_data->load_addr = addr;
      curr_item->addr = addr;
      addr += len;
    } else {
      break;
    }
  return last_item;
}

void MIR_load_module (MIR_context_t context, MIR_module_t m) {
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, m->items);
       item != NULL;
       item = DLIST_NEXT (MIR_item_t, item)) {
    if (item->item_type == MIR_bss_item || item->item_type == MIR_data_item
	|| item->item_type == MIR_ref_data_item || item->item_type == MIR_expr_data_item) {
      item = load_bss_data_section (context, item, FALSE);
    } else if (item->item_type == MIR_func_item) {
      if (item->addr == NULL)
	item->addr = _MIR_get_thunk (context, item);
      _MIR_redirect_thunk (context, item->addr, undefined_interface);
    }
    if (item->export_p) { /* update global item table */
      mir_assert (item->item_type != MIR_export_item
		  && item->item_type != MIR_import_item
		  && item->item_type != MIR_forward_item);
      setup_global (context, MIR_item_name (context, item), item->addr, item);
    }
  }
  VARR_PUSH (MIR_module_t, modules_to_link, m);
}

void MIR_load_external (MIR_context_t context, const char *name, void *addr) {
  setup_global (context, name, addr, NULL);
}

void MIR_link (MIR_context_t context,
	       void (*set_interface) (MIR_context_t context, MIR_item_t item),
	       void *import_resolver (const char *)) {
  MIR_item_t item, tab_item, def, expr_item;
  MIR_type_t type;
  MIR_val_t res;
  MIR_module_t m;
  void *addr;
  union {
    int8_t i8; int16_t i16; int32_t i32; int64_t i64; float f; double d; long double ld; void *a;
  } v;
  
  for (size_t i = 0; i < VARR_LENGTH (MIR_module_t, modules_to_link); i++) {
    m = VARR_GET (MIR_module_t, modules_to_link, i);
    for (item = DLIST_HEAD (MIR_item_t, m->items);
	 item != NULL;
	 item = DLIST_NEXT (MIR_item_t, item))
      if (item->item_type == MIR_import_item) {
	if ((tab_item = find_item (context, item->u.import, &environment_module)) == NULL) {
	  if (import_resolver == NULL || (addr = import_resolver (item->u.import)) == NULL)
	    (*error_func) (MIR_undeclared_op_ref_error, "import of undefined item %s", item->u.import);
	  MIR_load_external (context, item->u.import, addr);
	  tab_item = find_item (context, item->u.import, &environment_module);
	  mir_assert (tab_item != NULL);
	}
	item->addr = tab_item->addr;
	item->ref_def = tab_item;
      } else if (item->item_type == MIR_export_item) {
	if ((tab_item = find_item (context, item->u.export, m)) == NULL)
	  (*error_func) (MIR_undeclared_op_ref_error, "export of undefined item %s", item->u.export);
	item->addr = tab_item->addr;
	item->ref_def = tab_item;
      }
  }
  for (size_t i = 0; i < VARR_LENGTH (MIR_module_t, modules_to_link); i++) {
    m = VARR_GET (MIR_module_t, modules_to_link, i);
    for (item = DLIST_HEAD (MIR_item_t, m->items);
	 item != NULL;
	 item = DLIST_NEXT (MIR_item_t, item)) {
      if (item->item_type == MIR_ref_data_item) {
	assert (item->u.ref_data->ref_item->addr != NULL);
	memcpy (item->u.ref_data->load_addr,
		&item->u.ref_data->ref_item->addr, _MIR_type_size (context, MIR_T_P));
	continue;
      }
      if (item->item_type != MIR_expr_data_item)
	continue;
      expr_item = item->u.expr_data->expr_item;
      MIR_interp (context, expr_item, &res, 0);
      type = expr_item->u.func->res_types[0];
      switch (type) {
      case MIR_T_I8: case MIR_T_U8: v.i8 = (int8_t) res.i; break;
      case MIR_T_I16: case MIR_T_U16: v.i16 = (int16_t) res.i; break;
      case MIR_T_I32: case MIR_T_U32: v.i32 = (int32_t) res.i; break;
      case MIR_T_I64: case MIR_T_U64: v.i64 = (int64_t) res.i; break;
      case MIR_T_F: v.f = res.f; break;
      case MIR_T_D: v.d = res.d; break;
      case MIR_T_LD: v.ld = res.ld; break;
      case MIR_T_P: v.a = res.a; break;
      default: assert (FALSE); break;
      }
      memcpy (item->u.expr_data->load_addr, &v,
	      _MIR_type_size (context, expr_item->u.func->res_types[0]));
    }
  }
  if (set_interface != NULL)
    while (VARR_LENGTH (MIR_module_t, modules_to_link) != 0) {
      m = VARR_POP (MIR_module_t, modules_to_link);
      for (item = DLIST_HEAD (MIR_item_t, m->items);
	   item != NULL;
	   item = DLIST_NEXT (MIR_item_t, item))
	if (item->item_type == MIR_func_item) {
	  finish_func_interpretation (item); /* in case if it was used for expr data */
	  set_interface (context, item);
	}
    }
}

static const char *insn_name (MIR_insn_code_t code) {
  return code < 0 || code >= MIR_INSN_BOUND ? "" : insn_descs[code].name;
}

const char *MIR_insn_name (MIR_context_t context, MIR_insn_code_t code) {
  if (code < 0 || code >= MIR_INSN_BOUND)
    (*error_func) (MIR_wrong_param_value_error, "MIR_insn_name: wrong insn code %d", (int) code);
  return insn_descs[code].name;
}

static size_t insn_code_nops (MIR_context_t context, MIR_insn_code_t code) { /* 0 for calls */
  if (code < 0 || code >= MIR_INSN_BOUND)
    (*error_func) (MIR_wrong_param_value_error, "insn_code_nops: wrong insn code %d", (int) code);
  return VARR_GET (size_t, insn_nops, code);
}

size_t MIR_insn_nops (MIR_context_t context, MIR_insn_t insn) { return insn->nops; }

MIR_op_mode_t _MIR_insn_code_op_mode (MIR_context_t context, MIR_insn_code_t code,
				      size_t nop, int *out_p) {
  unsigned mode;

  if (nop >= insn_code_nops (context, code))
    return MIR_OP_BOUND;
  mode = insn_descs[code].op_modes[nop];
  *out_p = (mode & OUTPUT_FLAG) != 0;
  return *out_p ? mode ^ OUTPUT_FLAG : mode;
}

MIR_op_mode_t MIR_insn_op_mode (MIR_context_t context, MIR_insn_t insn, size_t nop, int *out_p) {
  MIR_insn_code_t code = insn->code;
  size_t nargs, nops = MIR_insn_nops (context, insn);
  unsigned mode;

  if (nop >= nops)
    return MIR_OP_BOUND;
  if (MIR_call_code_p (code)) {
    MIR_op_t proto_op = insn->ops[0];
    MIR_proto_t proto;

    mir_assert (proto_op.mode == MIR_OP_REF && proto_op.u.ref->item_type == MIR_proto_item);
    proto = proto_op.u.ref->u.proto;
    *out_p = 2 <= nop && nop < proto->nres + 2;
    nargs = proto->nres + 2 + (proto->args == NULL ? 0 : VARR_LENGTH (MIR_var_t, proto->args));
    if (proto->vararg_p && nop >= nargs)
      return MIR_OP_UNDEF; /* unknown */
    mir_assert (nops >= nargs && (proto->vararg_p || nops == nargs));
    return (nop == 0 ? insn->ops[nop].mode : nop == 1 ? MIR_OP_INT
	    : 2 <= nop && nop < proto->nres + 2 ? type2mode (proto->res_types[nop - 2])
	    : type2mode (VARR_GET (MIR_var_t, proto->args, nop - 2 - proto->nres).type));
  }
  mode = insn_descs[code].op_modes[nop];
  *out_p = (mode & OUTPUT_FLAG) != 0;
  return *out_p ? mode ^ OUTPUT_FLAG : mode;
}

static MIR_insn_t create_insn (MIR_context_t context, size_t nops, MIR_insn_code_t code) {
  MIR_insn_t insn;

  if (nops == 0)
    nops = 1;
  insn = malloc (sizeof (struct MIR_insn) + sizeof (MIR_op_t) * (nops - 1));
  if (insn == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for insn creation");
  insn->code = code; insn->data = NULL;
  return insn;
}

static MIR_insn_t new_insn1 (MIR_context_t context, MIR_insn_code_t code) {
  return create_insn (context, 1, code);
}

MIR_insn_t MIR_new_insn_arr (MIR_context_t context, MIR_insn_code_t code,
			     size_t nops, MIR_op_t *ops) {
  MIR_insn_t insn;
  MIR_proto_t proto;
  size_t i = 0, expected_nops = insn_code_nops (context, code);
  
  if  (! MIR_call_code_p (code) && code != MIR_RET && nops != expected_nops) {
    (*error_func) (MIR_ops_num_error, "wrong number of operands for insn %s", insn_descs[code].name);
  } else if (MIR_call_code_p (code)) {
    if (nops < 2)
      (*error_func) (MIR_ops_num_error, "wrong number of call operands");
    if (ops[0].mode != MIR_OP_REF || ops[0].u.ref->item_type != MIR_proto_item)
      (*error_func) (MIR_call_op_error, "the 1st call operand should be a prototype");
    proto = ops[0].u.ref->u.proto;
    i = proto->nres;
    if (proto->args != NULL)
      i += VARR_LENGTH (MIR_var_t, proto->args);
    if (nops < i + 2 || (nops != i + 2 && ! proto->vararg_p))
      (*error_func) (MIR_call_op_error,
		     "number of call operands or results does not correspond to prototype %s", proto->name);
  } else if (code == MIR_VA_ARG) {
    if (ops[2].mode != MIR_OP_MEM)
      (*error_func) (MIR_op_mode_error, "3rd operand of va_arg should be any memory with given type");
  }
  insn = create_insn (context, nops, code);
  insn->nops = nops;
  for (i = 0; i < nops; i++)
    insn->ops[i] = ops[i];
  return insn;
}

static MIR_insn_t new_insn (MIR_context_t context, MIR_insn_code_t code,
			    size_t nops, va_list argp) {
  VARR_TRUNC (MIR_op_t, temp_insn_ops, 0);
  for (size_t i = 0; i < nops; i++) {
    MIR_op_t op = va_arg (argp, MIR_op_t);

    VARR_PUSH (MIR_op_t, temp_insn_ops, op);
  }
  va_end (argp);
  return MIR_new_insn_arr (context, code, nops, VARR_ADDR (MIR_op_t, temp_insn_ops));
}

MIR_insn_t MIR_new_insn (MIR_context_t context, MIR_insn_code_t code, ...) {
  va_list argp;
  size_t nops = insn_code_nops (context, code);
  
  if (MIR_call_code_p (code) || code == MIR_RET)
    (*error_func) (MIR_call_op_error,
		   "Use only MIR_new_insn_arr or MIR_new_{call,ret}_insn for creating a call/ret insn");
  va_start (argp, code);
  return new_insn (context, code, nops, argp);
}

MIR_insn_t MIR_new_call_insn (MIR_context_t context, size_t nops, ...) {
  va_list argp;

  va_start (argp, nops);
  return new_insn (context, MIR_CALL, nops, argp);
}

MIR_insn_t MIR_new_ret_insn (MIR_context_t context, size_t nops, ...) {
  va_list argp;

  va_start (argp, nops);
  return new_insn (context, MIR_RET, nops, argp);
}

MIR_insn_t MIR_copy_insn (MIR_context_t context, MIR_insn_t insn) {
  size_t size = sizeof (struct MIR_insn) + sizeof (MIR_op_t) * (insn->nops - 1);
  MIR_insn_t new_insn = malloc (size);
  
  if (new_insn == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory to copy insn %s", insn_name (insn->code));
  memcpy (new_insn, insn, size);
  return new_insn;
  
}

static MIR_insn_t create_label (MIR_context_t context, int64_t label_num) {
  MIR_insn_t insn = new_insn1 (context, MIR_LABEL);

  insn->ops[0] = MIR_new_int_op (context, label_num);
  insn->nops = 0;
  return insn;
}

MIR_insn_t MIR_new_label (MIR_context_t context) {
  return create_label (context, ++curr_label_num);
}

MIR_reg_t _MIR_new_temp_reg (MIR_context_t context, MIR_type_t type, MIR_func_t func) {
  string_t string;

  if (type != MIR_T_I64 && type != MIR_T_F && type != MIR_T_D && type != MIR_T_LD)
    (*error_func) (MIR_reg_type_error, "wrong type %s for temporary register", type_str (type));
  for (;;) {
    func->last_temp_num++;
    if (func->last_temp_num == 0)
      (*error_func) (MIR_unique_reg_error, "out of unique regs");
    sprintf (temp_buff, "t%d", func->last_temp_num);
    string = string_store (context, &strings, &string_tab, temp_buff);
    if (find_rd_by_name_num (context, string.num, func) == NULL)
      return MIR_new_func_reg (context, func, type, string.str);
  }
}

static reg_desc_t *get_func_rd_by_name (MIR_context_t context,
					const char *reg_name, MIR_func_t func) {
  string_t string = string_store (context, &strings, &string_tab, reg_name);
  reg_desc_t *rd;
  
  rd = find_rd_by_name_num (context, string.num, func);
  if (rd == NULL)
    (*error_func) (MIR_undeclared_func_reg_error, "undeclared func reg %s", reg_name);
  return rd;
}

static reg_desc_t *get_func_rd_by_reg (MIR_context_t context, MIR_reg_t reg, MIR_func_t func) {
  reg_desc_t *rd;
  
  rd = find_rd_by_reg (context, reg, func);
  return rd;
}

MIR_reg_t MIR_reg (MIR_context_t context, const char *reg_name, MIR_func_t func) {
  return get_func_rd_by_name (context, reg_name, func)->reg;
}

MIR_type_t MIR_reg_type (MIR_context_t context, MIR_reg_t reg, MIR_func_t func) {
  return get_func_rd_by_reg (context, reg, func)->type;
}

const char *MIR_reg_name (MIR_context_t context, MIR_reg_t reg, MIR_func_t func) {
  return VARR_ADDR (string_t, strings) [get_func_rd_by_reg (context, reg, func)->name_num].str;
}

/* Functions to create operands.  */

static void init_op (MIR_op_t *op, MIR_op_mode_t mode) { op->mode = mode; op->data = NULL; }

MIR_op_t MIR_new_reg_op (MIR_context_t context, MIR_reg_t reg) {
  MIR_op_t op;

  init_op (&op, MIR_OP_REG); op.u.reg = reg;
  return op;
}

MIR_op_t _MIR_new_hard_reg_op (MIR_context_t context, MIR_reg_t hard_reg) { /* It is used only internally */
  MIR_op_t op;

  init_op (&op, MIR_OP_HARD_REG); op.u.hard_reg = hard_reg;
  return op;
}

MIR_op_t MIR_new_int_op (MIR_context_t context, int64_t i) {
  MIR_op_t op;

  init_op (&op, MIR_OP_INT); op.u.i = i;
  return op;
}

MIR_op_t MIR_new_uint_op (MIR_context_t context, uint64_t u) {
  MIR_op_t op;

  init_op (&op, MIR_OP_UINT); op.u.u = u;
  return op;
}

MIR_op_t MIR_new_float_op (MIR_context_t context, float f) {
  MIR_op_t op;

  mir_assert (sizeof (float) == 4); /* IEEE single */
  init_op (&op, MIR_OP_FLOAT); op.u.f = f;
  return op;
}

MIR_op_t MIR_new_double_op (MIR_context_t context, double d) {
  MIR_op_t op;

  mir_assert (sizeof (double) == 8); /* IEEE double */
  init_op (&op, MIR_OP_DOUBLE); op.u.d = d;
  return op;
}

MIR_op_t MIR_new_ldouble_op (MIR_context_t context, long double ld) {
  MIR_op_t op;

  mir_assert (sizeof (long double) == 16); /* machine-defined 80- or 128-bit FP  */
  init_op (&op, MIR_OP_LDOUBLE); op.u.ld = ld;
  return op;
}

MIR_op_t MIR_new_ref_op (MIR_context_t context, MIR_item_t item) {
  MIR_op_t op;

  init_op (&op, MIR_OP_REF); op.u.ref = item;
  return op;
}

MIR_op_t MIR_new_str_op (MIR_context_t context, const char *str) {
  MIR_op_t op;

  init_op (&op, MIR_OP_STR); op.u.str = string_store (context, &strings, &string_tab, str).str;
  return op;
}

MIR_op_t MIR_new_mem_op (MIR_context_t context, MIR_type_t type, MIR_disp_t disp,
			 MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale) {
  MIR_op_t op;

  init_op (&op, MIR_OP_MEM); op.u.mem.type = type; op.u.mem.disp = disp;
  op.u.mem.base = base; op.u.mem.index = index; op.u.mem.scale = scale;
  return op;
}

MIR_op_t _MIR_new_hard_reg_mem_op (MIR_context_t context, MIR_type_t type, MIR_disp_t disp,
				   MIR_reg_t base, MIR_reg_t index, MIR_scale_t scale) {
  MIR_op_t op;

  init_op (&op, MIR_OP_HARD_REG_MEM); op.u.hard_reg_mem.type = type; op.u.hard_reg_mem.disp = disp;
  op.u.hard_reg_mem.base = base; op.u.hard_reg_mem.index = index; op.u.hard_reg_mem.scale = scale;
  return op;
}

MIR_op_t MIR_new_label_op (MIR_context_t context, MIR_label_t label) {
  MIR_op_t op;

  init_op (&op, MIR_OP_LABEL); op.u.label = label;
  return op;
}

int MIR_op_eq_p (MIR_context_t context, MIR_op_t op1, MIR_op_t op2) {
  if (op1.mode != op2.mode)
    return FALSE;
  switch (op1.mode) {
  case MIR_OP_REG: return op1.u.reg == op2.u.reg;
  case MIR_OP_HARD_REG: return op1.u.hard_reg == op2.u.hard_reg;
  case MIR_OP_INT: return op1.u.i == op2.u.i;
  case MIR_OP_UINT: return op1.u.u == op2.u.u;
  case MIR_OP_FLOAT: return op1.u.f == op2.u.f;
  case MIR_OP_DOUBLE: return op1.u.d == op2.u.d;
  case MIR_OP_LDOUBLE: return op1.u.ld == op2.u.ld;
  case MIR_OP_REF: return strcmp (MIR_item_name (context, op1.u.ref),
				  MIR_item_name (context, op2.u.ref)) == 0;
  case MIR_OP_STR: return strcmp (op1.u.str, op2.u.str) == 0;
  case MIR_OP_MEM:
    return (op1.u.mem.type == op2.u.mem.type && op1.u.mem.disp == op2.u.mem.disp
	    && op1.u.mem.base == op2.u.mem.base && op1.u.mem.index == op2.u.mem.index
	    && (op1.u.mem.index == 0 || op1.u.mem.scale == op2.u.mem.scale));
  case MIR_OP_HARD_REG_MEM:
    return (op1.u.hard_reg_mem.type == op2.u.hard_reg_mem.type
	    && op1.u.hard_reg_mem.disp == op2.u.hard_reg_mem.disp
	    && op1.u.hard_reg_mem.base == op2.u.hard_reg_mem.base
	    && op1.u.hard_reg_mem.index == op2.u.hard_reg_mem.index
	    && (op1.u.hard_reg_mem.index == MIR_NON_HARD_REG
		|| op1.u.hard_reg_mem.scale == op2.u.hard_reg_mem.scale));
  case MIR_OP_LABEL: return op1.u.label == op2.u.label;
  default:
    mir_assert (FALSE); /* we should not have other operands here */
  }
  return FALSE;
}

htab_hash_t MIR_op_hash_step (MIR_context_t context, htab_hash_t h, MIR_op_t op) {
  h = mir_hash_step (h, (uint64_t) op.mode);
  switch (op.mode) {
  case MIR_OP_REG: return mir_hash_step (h, (uint64_t) op.u.reg);
  case MIR_OP_HARD_REG: return mir_hash_step (h, (uint64_t) op.u.hard_reg);
  case MIR_OP_INT: return mir_hash_step (h, (uint64_t) op.u.i);
  case MIR_OP_UINT: return mir_hash_step (h, (uint64_t) op.u.u);
  case MIR_OP_FLOAT: {
    union {double d; uint64_t u;} u;
    
    u.d = op.u.f;
    return mir_hash_step (h, u.u);
  }
  case MIR_OP_DOUBLE:
    return mir_hash_step (h, op.u.u);
  case MIR_OP_LDOUBLE: {
    union {long double ld; uint64_t u[2];} u;
    
    u.ld = op.u.ld;
    return mir_hash_step (mir_hash_step (h, u.u[0]), u.u[1]);
  }
  case MIR_OP_REF: return mir_hash_step (h, (uint64_t) MIR_item_name (context, op.u.ref));
  case MIR_OP_STR: return mir_hash_step (h, (uint64_t) op.u.str);
  case MIR_OP_MEM:
    h = mir_hash_step (h, (uint64_t) op.u.mem.type);
    h = mir_hash_step (h, (uint64_t) op.u.mem.disp);
    h = mir_hash_step (h, (uint64_t) op.u.mem.base);
    h = mir_hash_step (h, (uint64_t) op.u.mem.index);
    if (op.u.mem.index != 0)
      h = mir_hash_step (h, (uint64_t) op.u.mem.scale);
    break;
  case MIR_OP_HARD_REG_MEM:
    h = mir_hash_step (h, (uint64_t) op.u.hard_reg_mem.type);
    h = mir_hash_step (h, (uint64_t) op.u.hard_reg_mem.disp);
    h = mir_hash_step (h, (uint64_t) op.u.hard_reg_mem.base);
    h = mir_hash_step (h, (uint64_t) op.u.hard_reg_mem.index);
    if (op.u.hard_reg_mem.index != MIR_NON_HARD_REG)
      h = mir_hash_step (h, (uint64_t) op.u.hard_reg_mem.scale);
    break;
  case MIR_OP_LABEL: return mir_hash_step (h, (uint64_t) op.u.label);
  default:
    mir_assert (FALSE); /* we should not have other operands here */
  }
  return h;
}

void MIR_append_insn (MIR_context_t context, MIR_item_t func_item, MIR_insn_t insn) {
  if (func_item->item_type != MIR_func_item)
    (*error_func) (MIR_wrong_param_value_error, "MIR_append_insn: wrong func item");
  DLIST_APPEND (MIR_insn_t, func_item->u.func->insns, insn);
}

void MIR_prepend_insn (MIR_context_t context, MIR_item_t func_item, MIR_insn_t insn) {
  if (func_item->item_type != MIR_func_item)
    (*error_func) (MIR_wrong_param_value_error, "MIR_prepend_insn: wrong func item");
  DLIST_PREPEND (MIR_insn_t, func_item->u.func->insns, insn);
}

void MIR_insert_insn_after (MIR_context_t context, MIR_item_t func_item,
			    MIR_insn_t after, MIR_insn_t insn) {
  if (func_item->item_type != MIR_func_item)
    (*error_func) (MIR_wrong_param_value_error, "MIR_insert_insn_after: wrong func item");
  DLIST_INSERT_AFTER (MIR_insn_t, func_item->u.func->insns, after, insn);
}

void MIR_insert_insn_before (MIR_context_t context, MIR_item_t func_item,
			     MIR_insn_t before, MIR_insn_t insn) {
  if (func_item->item_type != MIR_func_item)
    (*error_func) (MIR_wrong_param_value_error, "MIR_insert_insn_before: wrong func item");
  DLIST_INSERT_BEFORE (MIR_insn_t, func_item->u.func->insns, before, insn);
}

void MIR_remove_insn (MIR_context_t context, MIR_item_t func_item, MIR_insn_t insn) {
  if (func_item->item_type != MIR_func_item)
    (*error_func) (MIR_wrong_param_value_error, "MIR_remove_insn: wrong func item");
  DLIST_REMOVE (MIR_insn_t, func_item->u.func->insns, insn);
  free (insn);
}

static void output_type (FILE *f, MIR_type_t tp) { fprintf (f, "%s", MIR_type_str (NULL, tp)); }

static void output_disp (FILE *f, MIR_disp_t disp) { fprintf (f, "%" PRId64, (int64_t) disp); }

static void output_scale (FILE *f, unsigned scale) { fprintf (f, "%u", scale); }

static void output_reg (MIR_context_t context, FILE *f, MIR_func_t func, MIR_reg_t reg) {
  fprintf (f, "%s", MIR_reg_name (context, reg, func));
}

static void output_hard_reg (FILE *f, MIR_reg_t reg) { fprintf (f, "hr%u", reg); }

static void output_label (MIR_context_t context, FILE *f, MIR_func_t func, MIR_label_t label);

static void out_str (FILE *f, const char *str) {
  fprintf (f, "\"");
  for (size_t i = 0; str[i] != '\0'; i++)
    if (str[i] == '\\')
      fprintf (f, "\\");
    else if (str[i] == '"')
      fprintf (f, "\"");
    else if (isprint (str[i]))
      fprintf (f, "%c", str[i]);
    else if (str[i] == '\n')
      fprintf (f, "\\n");
    else if (str[i] == '\t')
      fprintf (f, "\\t");
    else if (str[i] == '\v')
      fprintf (f, "\\v");
    else if (str[i] == '\a')
      fprintf (f, "\\a");
    else if (str[i] == '\b')
      fprintf (f, "\\b");
    else if (str[i] == '\f')
      fprintf (f, "\\f");
    else
      fprintf (f, "\\%03o", str[i]);
  fprintf (f, "\"");
}

void MIR_output_op (MIR_context_t context, FILE *f, MIR_op_t op, MIR_func_t func) {
  switch (op.mode) {
  case MIR_OP_REG:
    output_reg (context, f, func, op.u.reg);
    break;
  case MIR_OP_HARD_REG:
    output_hard_reg (f, op.u.hard_reg);
    break;
  case MIR_OP_INT:
    fprintf (f, "%" PRId64, op.u.i);
    break;
  case MIR_OP_UINT:
    fprintf (f, "%" PRIu64, op.u.u);
    break;
  case MIR_OP_FLOAT:
    fprintf (f, "%.*ef", FLT_DECIMAL_DIG, op.u.f);
    break;
  case MIR_OP_DOUBLE:
    fprintf (f, "%.*e", DBL_DECIMAL_DIG, op.u.d);
    break;
  case MIR_OP_LDOUBLE:
    fprintf (f, "%.*Le", LDBL_DECIMAL_DIG, op.u.ld);
    break;
  case MIR_OP_MEM:
  case MIR_OP_HARD_REG_MEM: {
    MIR_reg_t no_reg = op.mode == MIR_OP_MEM ? 0 : MIR_NON_HARD_REG;
    
    output_type (f, op.u.mem.type);
    fprintf (f, ":");
    if (op.u.mem.disp != 0 || (op.u.mem.base == no_reg && op.u.mem.index == no_reg))
      output_disp (f, op.u.mem.disp);
    if (op.u.mem.base != no_reg || op.u.mem.index != no_reg) {
      fprintf (f, "(");
      if (op.u.mem.base != no_reg) {
	if (op.mode == MIR_OP_MEM)
	  output_reg (context, f, func, op.u.mem.base);
	else
	  output_hard_reg (f, op.u.hard_reg_mem.base);
      }
      if (op.u.mem.index != no_reg) {
	fprintf(f, ", ");
	if (op.mode == MIR_OP_MEM)
	  output_reg (context, f, func, op.u.mem.index);
	else
	  output_hard_reg (f, op.u.hard_reg_mem.index);
	if (op.u.mem.scale != 1) {
	  fprintf(f, " * ");
	  output_scale (f, op.u.mem.scale);
	}
      }
      fprintf (f, ")");
    }
    break;
  }
  case MIR_OP_REF:
    fprintf (f, "%s", MIR_item_name (context, op.u.ref));
    break;
  case MIR_OP_STR:
    out_str (f, op.u.str);
    break;
  case MIR_OP_LABEL:
    output_label (context, f, func, op.u.label);
    break;
  default:
    mir_assert (FALSE);
  }
}

static void output_label (MIR_context_t context, FILE *f, MIR_func_t func, MIR_label_t label) {
  fprintf (f, "L"); MIR_output_op (context, f, label->ops[0], func);
}

void MIR_output_insn (MIR_context_t context, FILE *f, MIR_insn_t insn, MIR_func_t func, int newline_p) {
  size_t i, nops;
  
  if (insn->code == MIR_LABEL) {
    output_label (context, f, func, insn);
    if (newline_p)
      fprintf (f, ":\n");
    return;
  }
  fprintf (f, "\t%s", MIR_insn_name (context, insn->code));
  nops = MIR_insn_nops (context, insn);
  for (i = 0; i < nops; i++) {
    fprintf (f, i == 0 ? "\t" : ", ");
    MIR_output_op (context, f, insn->ops[i], func);
  }
  if (newline_p)
    fprintf (f, "\n");
}

static void output_func_proto (FILE *f, size_t nres, MIR_type_t *types,
			       size_t nargs, VARR (MIR_var_t) *args, int vararg_p) {
  size_t i;
  MIR_var_t var;
  
  for (i = 0; i < nres; i++) {
    if (i != 0)
      fprintf (f, ", ");
    fprintf (f, "%s", MIR_type_str (NULL, types[i]));
  }
  for (i = 0; i < nargs; i++) {
    var = VARR_GET (MIR_var_t, args, i);
    if (i != 0 || nres != 0)
      fprintf (f, ", ");
    mir_assert (var.name != NULL);
    fprintf (f, "%s:%s", MIR_type_str (NULL, var.type), var.name);
  }
  if (vararg_p)
    fprintf (f, nargs == 0 && nres == 0 ? "..." : ", ...");
  fprintf (f, "\n");
}

static void output_item (MIR_context_t context, FILE *f, MIR_item_t item) {
  MIR_insn_t insn;
  MIR_func_t func;
  MIR_proto_t proto;
  MIR_var_t var;
  MIR_data_t data;
  MIR_ref_data_t ref_data;
  MIR_expr_data_t expr_data;
  size_t i, nlocals;
  
  if (item->item_type == MIR_export_item) {
    fprintf (f, "\texport\t%s\n", item->u.export);
    return;
  }
  if (item->item_type == MIR_import_item) {
    fprintf (f, "\timport\t%s\n", item->u.import);
    return;
  }
  if (item->item_type == MIR_forward_item) {
    fprintf (f, "\tforward\t%s\n", item->u.forward);
    return;
  }
  if (item->item_type == MIR_bss_item) {
    if (item->u.bss->name != NULL)
      fprintf (f, "%s:", item->u.bss->name);
    fprintf (f, "\tbss\t%" PRIu64 "\n", item->u.bss->len);
    return;
  }
  if (item->item_type == MIR_ref_data_item) {
    ref_data = item->u.ref_data;
    if (ref_data->name != NULL)
      fprintf (f, "%s:", ref_data->name);
    fprintf (f, "\tref\t%s\n", MIR_item_name (context, ref_data->ref_item));
    return;
  }
  if (item->item_type == MIR_expr_data_item) {
    expr_data = item->u.expr_data;
    if (expr_data->name != NULL)
      fprintf (f, "%s:", expr_data->name);
    fprintf (f, "\texpr\t%s", MIR_item_name (context, expr_data->expr_item));
  }
  if (item->item_type == MIR_data_item) {
    data = item->u.data;
    if (data->name != NULL)
      fprintf (f, "%s:", data->name);
    fprintf (f, "\t%s\t", MIR_type_str (NULL, data->el_type));
    for (size_t i = 0; i < data->nel; i++) {
      switch (data->el_type) {
      case MIR_T_I8: fprintf (f, "%" PRId8, ((int8_t *) data->u.els)[i]); break;
      case MIR_T_U8: fprintf (f, "%" PRIu8, ((uint8_t *) data->u.els)[i]); break;
      case MIR_T_I16: fprintf (f, "%" PRId16, ((int16_t *) data->u.els)[i]); break;
      case MIR_T_U16: fprintf (f, "%" PRIu16, ((uint16_t *) data->u.els)[i]); break;
      case MIR_T_I32: fprintf (f, "%" PRId32, ((int32_t *) data->u.els)[i]); break;
      case MIR_T_U32: fprintf (f, "%" PRIu32, ((uint32_t *) data->u.els)[i]); break;
      case MIR_T_I64: fprintf (f, "%" PRId64, ((int64_t *) data->u.els)[i]); break;
      case MIR_T_U64: fprintf (f, "%" PRIu64, ((uint64_t *) data->u.els)[i]); break;
      case MIR_T_F: fprintf (f, "%.*ef", FLT_DECIMAL_DIG, ((float *) data->u.els)[i]); break;
      case MIR_T_D: fprintf (f, "%.*e", DBL_DECIMAL_DIG, ((double *) data->u.els)[i]); break;
      case MIR_T_LD: fprintf (f, "%.*Le", LDBL_DECIMAL_DIG, ((long double *) data->u.els)[i]); break;
	/* only ptr as ref ??? */
      case MIR_T_P: fprintf (f, "0x%" PRIxPTR, ((uintptr_t *) data->u.els)[i]); break;
      default: mir_assert (FALSE);
      }
      if (i + 1 < data->nel)
	fprintf (f, ", ");
    }
    if (data->el_type == MIR_T_U8 && data->nel != 0 && data->u.els[data->nel - 1] == '\0') {
      fprintf (f, " # "); /* print possible string as a comment */
      out_str (f, (char *) data->u.els);
    }
    fprintf (f, "\n");
    return;
  }
  if (item->item_type == MIR_proto_item) {
    proto = item->u.proto;
    fprintf (f, "%s:\tproto\t", proto->name);
    output_func_proto (f, proto->nres, proto->res_types,
		       VARR_LENGTH (MIR_var_t, proto->args), proto->args, proto->vararg_p);
    return;
  }
  func = item->u.func;
  fprintf (f, "%s:\tfunc\t", func->name);
  output_func_proto (f, func->nres, func->res_types, func->nargs, func->vars, func->vararg_p);
  nlocals = VARR_LENGTH (MIR_var_t, func->vars) - func->nargs;
  for (i = 0; i < nlocals; i++) {
    var = VARR_GET (MIR_var_t, func->vars, i + func->nargs);
    if (i % 8 == 0) {
      if (i != 0)
	fprintf (f, "\n");
      fprintf (f, "\tlocal\t");
    }
    fprintf (f, i % 8 == 0 ? "%s:%s" : ", %s:%s", MIR_type_str (NULL, var.type), var.name);
  }
  fprintf (f, "\n# %u arg%s, %u local%s\n",
	   func->nargs, func->nargs == 1 ? "" : "s", (unsigned) nlocals, nlocals == 1 ? "" : "s");
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = DLIST_NEXT (MIR_insn_t, insn))
    MIR_output_insn (context, f, insn, func, TRUE);
  fprintf (f, "\tendfunc\n");
}

static void output_module (MIR_context_t context, FILE *f, MIR_module_t module) {
  fprintf (f, "%s:\tmodule\n", module->name);
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, module->items);
       item != NULL;
       item = DLIST_NEXT (MIR_item_t, item))
    output_item (context, f, item);
  fprintf (f, "\tendmodule\n");
}

void MIR_output (MIR_context_t context, FILE *f) {
  for (MIR_module_t module = DLIST_HEAD (MIR_module_t, all_modules);
       module != NULL;
       module = DLIST_NEXT (MIR_module_t, module))
    output_module (context, f, module);
}

static MIR_insn_t insert_op_insn (MIR_context_t context, int out_p, MIR_item_t func_item,
				  MIR_insn_t anchor, MIR_insn_t insn) {
  if (! out_p) {
    MIR_insert_insn_before (context, func_item, anchor, insn);
    return anchor;
  }
  MIR_insert_insn_after (context, func_item, anchor, insn);
  return insn;
}

typedef struct {
  MIR_insn_code_t code;
  MIR_type_t type;
  MIR_op_t op1, op2;
  MIR_reg_t reg;
  MIR_context_t context;
} val_t;

DEF_HTAB (val_t);

struct simplify_context {
  HTAB (val_t) *val_tab;
};

#define val_tab context->simplify_context->val_tab

static htab_hash_t val_hash (val_t v) {
  htab_hash_t h;

  h = mir_hash_step (mir_hash_init (0), (uint64_t) v.code);
  h = mir_hash_step (h, (uint64_t) v.type);
  h = MIR_op_hash_step (v.context, h, v.op1);
  if (v.code != MIR_INSN_BOUND)
    h = MIR_op_hash_step (v.context, h, v.op2);
  return mir_hash_finish (h);
}

static int val_eq (val_t v1, val_t v2) {
  assert (v1.context == v2.context);
  if (v1.code != v2.code || v1.type != v2.type || ! MIR_op_eq_p (v1.context, v1.op1, v2.op1))
    return FALSE;
  return v1.code == MIR_INSN_BOUND || MIR_op_eq_p (v1.context, v1.op2, v2.op2);
}

static void vn_init (MIR_context_t context) {
  if ((context->simplify_context = malloc (sizeof (struct simplify_context))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for context");
  HTAB_CREATE (val_t, val_tab, 512, val_hash, val_eq);
}

static void vn_finish (MIR_context_t context) {
  HTAB_DESTROY (val_t, val_tab);
  free (context->simplify_context); context->simplify_context = NULL;
}

static void vn_empty (MIR_context_t context) {
  HTAB_CLEAR (val_t, val_tab, NULL);
}

static MIR_reg_t vn_add_val (MIR_context_t context, MIR_func_t func, MIR_type_t type,
			     MIR_insn_code_t code, MIR_op_t op1, MIR_op_t op2) {
  val_t val, tab_val;

  val.type = type; val.code = code; val.op1 = op1; val.op2 = op2; val.context = context;
  if (HTAB_DO (val_t, val_tab, val, HTAB_FIND, tab_val))
    return tab_val.reg;
  val.reg = _MIR_new_temp_reg (context, type, func);
  HTAB_DO (val_t, val_tab, val, HTAB_INSERT, tab_val);
  return val.reg;
}

const char *_MIR_get_temp_item_name (MIR_context_t context, MIR_module_t module) {
  snprintf (temp_buff, sizeof (temp_buff), "%s%lu",
	    TEMP_ITEM_NAME_PREFIX, (unsigned long) module->temp_items_num);
  module->temp_items_num++;
  return temp_buff;
}

void MIR_simplify_op (MIR_context_t context, MIR_item_t func_item, MIR_insn_t insn, int nop,
		      int out_p, MIR_insn_code_t code, int mem_float_p) {
  MIR_op_t new_op, mem_op, *op = &insn->ops[nop];
  MIR_insn_t new_insn;
  MIR_func_t func = func_item->u.func;
  MIR_type_t type;
  MIR_op_mode_t value_mode = op->value_mode;
  int move_p = code == MIR_MOV || code == MIR_FMOV || code == MIR_DMOV || code == MIR_LDMOV;
  
  if (MIR_call_code_p (code)) {
    if (nop == 0)
      return; /* do nothing: it is a prototype */
    if (nop == 1 && op->mode == MIR_OP_REF
	&& (op->u.ref->item_type == MIR_import_item || op->u.ref->item_type == MIR_func_item))
      return; /* do nothing: it is an immediate oeprand */
  }
  if (code == MIR_VA_ARG && nop == 2)
    return; /* do nothing: this operand is used as a type */
  switch (op->mode) {
  case MIR_OP_INT:
  case MIR_OP_UINT:
  case MIR_OP_FLOAT:
  case MIR_OP_DOUBLE:
  case MIR_OP_LDOUBLE:
  case MIR_OP_REF:
  case MIR_OP_STR:
    mir_assert (! out_p);
    if (op->mode == MIR_OP_REF) {
      for (MIR_item_t item = op->u.ref; item != NULL; item = item->ref_def)
	if (item->item_type != MIR_export_item && item->item_type != MIR_forward_item) {
	  op->u.ref = item;
	  break;
	}
    } else if (op->mode == MIR_OP_STR
	       || (mem_float_p && (op->mode == MIR_OP_FLOAT
				   || op->mode == MIR_OP_DOUBLE || op->mode == MIR_OP_LDOUBLE))) {
      const char *name;
      MIR_item_t item;
      MIR_module_t m = curr_module;
      
      curr_module = func_item->module;
      name = _MIR_get_temp_item_name (context, curr_module);
      if (op->mode == MIR_OP_STR) {
	item = MIR_new_string_data (context, name, op->u.str);
	*op = MIR_new_ref_op (context, item);
      } else {
	if (op->mode == MIR_OP_FLOAT)
	  item = MIR_new_data (context, name, MIR_T_F, 1, (uint8_t *) &op->u.f);
	else if (op->mode == MIR_OP_DOUBLE)
	  item = MIR_new_data (context, name, MIR_T_D, 1, (uint8_t *) &op->u.d);
	else
	  item = MIR_new_data (context, name, MIR_T_LD, 1, (uint8_t *) &op->u.ld);
	type = op->mode == MIR_OP_FLOAT ? MIR_T_F : op->mode == MIR_OP_DOUBLE ? MIR_T_D : MIR_T_LD;
	*op = MIR_new_ref_op (context, item);
	new_op = MIR_new_reg_op (context, vn_add_val (context, func, MIR_T_I64, MIR_INSN_BOUND, *op, *op));
	MIR_insert_insn_before (context, func_item, insn, MIR_new_insn (context, MIR_MOV, new_op, *op));
	*op = MIR_new_mem_op (context, type, 0, new_op.u.reg, 0, 1);
      }
      if (func_item->addr != NULL) /* The function was already loaded: we should load new data */
	load_bss_data_section (context, item, TRUE);
      curr_module = m;
    }
    if (move_p)
      return;
    type = (op->mode == MIR_OP_FLOAT ? MIR_T_F : op->mode == MIR_OP_DOUBLE ? MIR_T_D
	    : op->mode == MIR_OP_LDOUBLE ? MIR_T_LD
	    : op->mode == MIR_OP_MEM ? op->u.mem.type : MIR_T_I64);
    new_op = MIR_new_reg_op (context, vn_add_val (context, func, type, MIR_INSN_BOUND, *op, *op));
    MIR_insert_insn_before (context, func_item, insn,
			    MIR_new_insn (context,
					  type == MIR_T_F ? MIR_FMOV : type == MIR_T_D ? MIR_DMOV
					  : type == MIR_T_LD ? MIR_LDMOV : MIR_MOV,
					  new_op, *op));
    *op = new_op;
    break;
  case MIR_OP_REG:
  case MIR_OP_HARD_REG:
  case MIR_OP_LABEL:
    break; /* Do nothing */
  case MIR_OP_MEM: {
    MIR_reg_t addr_reg = 0;
    
    mem_op = *op;
    type = mem_op.u.mem.type;
    if (op->u.mem.base != 0 && op->u.mem.disp == 0 && (op->u.mem.index == 0 || op->u.mem.scale == 0)) {
      addr_reg = op->u.mem.base;
    } else if (op->u.mem.base == 0 && op->u.mem.index != 0 && op->u.mem.scale == 1 && op->u.mem.disp == 0) {
      addr_reg = op->u.mem.index;
    } else {
      int after_p = ! move_p && out_p;
      MIR_reg_t disp_reg = 0, scale_ind_reg = op->u.mem.index, base_reg = op->u.mem.base, base_ind_reg = 0;
      
      if (op->u.mem.disp != 0) {
	MIR_op_t disp_op = MIR_new_int_op (context, op->u.mem.disp);

	disp_reg = vn_add_val (context, func, MIR_T_I64, MIR_INSN_BOUND, disp_op, disp_op);
	insn = insert_op_insn (context, after_p, func_item, insn,
			       MIR_new_insn (context, MIR_MOV, MIR_new_reg_op (context, disp_reg), disp_op));
      }
      if (scale_ind_reg != 0 && op->u.mem.scale > 1) {
	MIR_op_t ind_op = MIR_new_reg_op (context, op->u.mem.index);
	MIR_op_t scale_reg_op, scale_int_op = MIR_new_int_op (context, op->u.mem.scale);

	scale_reg_op = MIR_new_reg_op (context, vn_add_val (context, func, MIR_T_I64,
							    MIR_INSN_BOUND, scale_int_op, scale_int_op));
	insn = insert_op_insn (context, after_p, func_item, insn,
			       MIR_new_insn (context, MIR_MOV, scale_reg_op, scale_int_op));
	scale_ind_reg = vn_add_val (context, func, MIR_T_I64, MIR_MUL, ind_op, scale_reg_op);
	insn = insert_op_insn (context, after_p, func_item, insn,
			       MIR_new_insn (context, MIR_MUL,
					     MIR_new_reg_op (context, scale_ind_reg), ind_op, scale_reg_op));
      }
      if (base_reg != 0 && scale_ind_reg != 0) {
	MIR_op_t base_op = MIR_new_reg_op (context, base_reg), ind_op = MIR_new_reg_op (context, scale_ind_reg);

	base_ind_reg = vn_add_val (context, func, MIR_T_I64, MIR_ADD, base_op, ind_op);
	insn = insert_op_insn (context, after_p, func_item, insn,
			       MIR_new_insn (context, MIR_ADD,
					     MIR_new_reg_op (context, base_ind_reg), base_op, ind_op));
      } else {
	base_ind_reg = base_reg != 0 ? base_reg : scale_ind_reg;
      }
      if (base_ind_reg == 0) {
	mir_assert (disp_reg != 0);
	addr_reg = disp_reg;
      } else if (disp_reg == 0) {
	mir_assert (base_ind_reg != 0);
	addr_reg = base_ind_reg;
      } else {
	MIR_op_t base_ind_op = MIR_new_reg_op (context, base_ind_reg), disp_op = MIR_new_reg_op (context, disp_reg);
	
	addr_reg = vn_add_val (context, func, MIR_T_I64, MIR_ADD, base_ind_op, disp_op);
	insn = insert_op_insn (context, after_p, func_item, insn,
			       MIR_new_insn (context, MIR_ADD,
					     MIR_new_reg_op (context, addr_reg), base_ind_op, disp_op));
      }
    }
    mem_op.u.mem.base = addr_reg;
    mem_op.u.mem.disp = 0; mem_op.u.mem.index = 0; mem_op.u.mem.scale = 0;
    if (move_p && (nop == 1 || insn->ops[1].mode == MIR_OP_REG)) {
      *op = mem_op;
    } else {
      type = (mem_op.u.mem.type == MIR_T_F || mem_op.u.mem.type == MIR_T_D || mem_op.u.mem.type == MIR_T_LD
	      ? mem_op.u.mem.type : MIR_T_I64);
      code = type == MIR_T_F ? MIR_FMOV : type == MIR_T_D ? MIR_DMOV : type == MIR_T_LD ? MIR_LDMOV : MIR_MOV;
      new_op = MIR_new_reg_op (context, vn_add_val (context, func, type, MIR_INSN_BOUND, mem_op, mem_op));
      if (out_p)
	new_insn = MIR_new_insn (context, code, mem_op, new_op);
      else
	new_insn = MIR_new_insn (context, code, new_op, mem_op);
      insn = insert_op_insn (context, out_p, func_item, insn, new_insn);
      *op = new_op;
    }
    break;
  }
  default:
    /* We don't simplify code with hard regs.  */
    mir_assert (FALSE);
  }
  op->value_mode = value_mode;
}

void _MIR_simplify_insn (MIR_context_t context, MIR_item_t func_item,
			 MIR_insn_t insn, int mem_float_p) {
  int out_p;
  MIR_insn_code_t code = insn->code;
  size_t i, nops = MIR_insn_nops (context, insn);

  for (i = 0; i < nops; i++) {
    MIR_insn_op_mode (context, insn, i, &out_p);
    MIR_simplify_op (context, func_item, insn, i, out_p, code, mem_float_p);
  }
}

static void make_one_ret (MIR_context_t context, MIR_item_t func_item) {
  size_t i, j;
  MIR_insn_code_t mov_code, ext_code;
  MIR_reg_t ret_reg;
  MIR_op_t reg_op, ret_reg_op;
  MIR_func_t func = func_item->u.func;
  MIR_type_t *res_types = func->res_types;
  MIR_insn_t ret_label, insn;
  VARR (MIR_op_t) *ret_ops;

  if (VARR_LENGTH (MIR_insn_t, temp_insns) == 1
      && VARR_GET (MIR_insn_t, temp_insns, 0) == DLIST_TAIL (MIR_insn_t, func->insns))
    return;
  ret_label = NULL;
  if (VARR_LENGTH (MIR_insn_t, temp_insns) != 0) {
    ret_label = MIR_new_label (context);
    MIR_append_insn (context, func_item, ret_label);
  }
  VARR_CREATE (MIR_op_t, ret_ops, 16);
  for (i = 0; i < func->nres; i++) {
    mov_code = (res_types[i] == MIR_T_F ? MIR_FMOV : res_types[i] == MIR_T_D ? MIR_DMOV
		: res_types[i] == MIR_T_LD ? MIR_LDMOV : MIR_MOV);
    ret_reg = _MIR_new_temp_reg (context, mov_code == MIR_MOV ? MIR_T_I64 : res_types[i], func);
    ret_reg_op = MIR_new_reg_op (context, ret_reg);
    VARR_PUSH (MIR_op_t, ret_ops, ret_reg_op);
    switch (res_types[i]) {
    case MIR_T_I8: ext_code = MIR_EXT8; break;
    case MIR_T_U8: ext_code = MIR_UEXT8; break;
    case MIR_T_I16: ext_code = MIR_EXT16; break;
    case MIR_T_U16: ext_code = MIR_UEXT16; break;
    case MIR_T_I32: ext_code = MIR_EXT32; break;
    case MIR_T_U32: ext_code = MIR_UEXT32; break;
    default: ext_code = MIR_INVALID_INSN; break;
    }
    if (ext_code != MIR_INVALID_INSN)
      MIR_append_insn (context, func_item,
		       MIR_new_insn (context, ext_code, ret_reg_op, ret_reg_op));
  }
  MIR_append_insn (context, func_item,
		   MIR_new_insn_arr (context, MIR_RET, func->nres, VARR_ADDR (MIR_op_t, ret_ops)));
  for (i = 0; i < VARR_LENGTH (MIR_insn_t, temp_insns); i++) {
    insn = VARR_GET (MIR_insn_t, temp_insns, i);
    mir_assert (func->nres == MIR_insn_nops (context, insn));
    for (j = 0; j < func->nres; j++) {
      mov_code = (res_types[j] == MIR_T_F ? MIR_FMOV : res_types[j] == MIR_T_D ? MIR_DMOV
		  : res_types[j] == MIR_T_LD ? MIR_LDMOV : MIR_MOV);
      reg_op = insn->ops[j];
      mir_assert (reg_op.mode == MIR_OP_REG);
      ret_reg_op = VARR_GET (MIR_op_t, ret_ops, j);
      MIR_insert_insn_before (context, func_item, insn, MIR_new_insn (context, mov_code, ret_reg_op, reg_op));
    }
    MIR_insert_insn_before (context, func_item, insn,
			    MIR_new_insn (context, MIR_JMP, MIR_new_label_op (context, ret_label)));
    MIR_remove_insn (context, func_item, insn);
  }
  VARR_DESTROY (MIR_op_t, ret_ops);
}

static int64_t natural_alignment (int64_t s) { return s <= 2 ? s : s <= 4 ? 4 : s <= 8 ? 8 : 16; }

void MIR_simplify_func (MIR_context_t context, MIR_item_t func_item, int mem_float_p) {
  MIR_func_t func = func_item->u.func;
  MIR_insn_t insn, next_insn, new_insn;
  MIR_reg_t reg;
  MIR_insn_code_t ext_code;
  
  if (func_item->item_type != MIR_func_item)
    (*error_func) (MIR_wrong_param_value_error, "MIR_remove_simplify: wrong func item");
  vn_empty (context);
  func = func_item->u.func;
  for (size_t i = 0; i < func->nargs; i++) {
    MIR_var_t var = VARR_GET (MIR_var_t, func->vars, i);
    
    if (var.type == MIR_T_I64 || var.type == MIR_T_U64
	|| var.type == MIR_T_F || var.type == MIR_T_D || var.type == MIR_T_LD)
      continue;
    switch (var.type) {
    case MIR_T_I8: ext_code = MIR_EXT8; break;
    case MIR_T_U8: ext_code = MIR_UEXT8; break;
    case MIR_T_I16: ext_code = MIR_EXT16; break;
    case MIR_T_U16: ext_code = MIR_UEXT16; break;
    case MIR_T_I32: ext_code = MIR_EXT32; break;
    case MIR_T_U32: ext_code = MIR_UEXT32; break;
    default: ext_code = MIR_INVALID_INSN; break;
    }
    if (ext_code != MIR_INVALID_INSN) {
      MIR_reg_t reg = MIR_reg (context, var.name, func);
      MIR_insn_t new_insn = MIR_new_insn (context, ext_code, MIR_new_reg_op (context, reg), MIR_new_reg_op (context, reg));
      
      MIR_prepend_insn (context, func_item, new_insn);
    }
  }
  VARR_TRUNC (MIR_insn_t, temp_insns, 0);
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = next_insn) {
    MIR_insn_code_t code = insn->code;
    MIR_op_t temp_op;
    
    if ((code == MIR_MOV || code == MIR_FMOV || code == MIR_DMOV || code == MIR_LDMOV)
	&& insn->ops[0].mode == MIR_OP_MEM && insn->ops[1].mode == MIR_OP_MEM) {
      temp_op = MIR_new_reg_op (context, _MIR_new_temp_reg (context,
							    code == MIR_MOV ? MIR_T_I64
							    : code == MIR_FMOV ? MIR_T_F
							    : code == MIR_DMOV ? MIR_T_D : MIR_T_LD, func));
      MIR_insert_insn_after (context, func_item, insn, MIR_new_insn (context, code, insn->ops[0], temp_op));
      insn->ops[0] = temp_op;
    }
    if (code == MIR_RET)
      VARR_PUSH (MIR_insn_t, temp_insns, insn);
    next_insn = DLIST_NEXT (MIR_insn_t, insn);
    if (code == MIR_ALLOCA
	&& (insn->ops[1].mode == MIR_OP_INT || insn->ops[1].mode == MIR_OP_UINT)) { /* consolidate allocas */
      int64_t size, overall_size, align, max_align;
      
      size = insn->ops[1].u.i;
      overall_size = size <= 0 ? 1 : size;
      max_align = align = natural_alignment (overall_size);
      overall_size = (overall_size + align - 1) / align * align;
      while (next_insn != NULL && next_insn->code == MIR_ALLOCA
	     && (next_insn->ops[1].mode == MIR_OP_INT || next_insn->ops[1].mode == MIR_OP_UINT)
	     && ! MIR_op_eq_p (context, insn->ops[0], next_insn->ops[0])) {
	size = next_insn->ops[1].u.i;
	size = size <= 0 ? 1 : size;
	align = natural_alignment (size);
	size = (size + align - 1) / align * align;
	if (max_align < align) {
	  max_align = align;
	  overall_size = (overall_size + align - 1) / align * align;
	}
	new_insn = MIR_new_insn (context, MIR_PTR32 ? MIR_ADDS : MIR_ADD, next_insn->ops[0],
				 insn->ops[0], MIR_new_int_op (context, overall_size));
	overall_size += size;
	MIR_insert_insn_before (context, func_item, next_insn, new_insn);
	MIR_remove_insn (context, func_item, next_insn);
	next_insn = DLIST_NEXT (MIR_insn_t, new_insn);
      }
      insn->ops[1].u.i = overall_size;
      next_insn = DLIST_NEXT (MIR_insn_t, insn); /* to process the current and new insns */
    }
    if (MIR_branch_code_p (code) && insn->ops[0].mode == MIR_OP_LABEL && insn->ops[0].u.label == next_insn
	&& ! MIR_FP_branch_code_p (code)) { /* remember signaling NAN */
      MIR_remove_insn (context, func_item, insn);
    } else if (((code == MIR_MUL || code == MIR_MULS || code == MIR_DIV || code == MIR_DIVS)
		&& insn->ops[2].mode == MIR_OP_INT && insn->ops[2].u.i == 1)
	       || ((code == MIR_ADD || code == MIR_ADDS || code == MIR_SUB || code == MIR_SUBS
		    || code == MIR_OR || code == MIR_ORS || code == MIR_XOR || code == MIR_XORS
		    || code == MIR_LSH || code == MIR_LSHS || code == MIR_RSH || code == MIR_RSHS
		    || code == MIR_URSH || code == MIR_URSHS)
		   && insn->ops[2].mode == MIR_OP_INT && insn->ops[2].u.i == 0)) {
      if (! MIR_op_eq_p (context, insn->ops[0], insn->ops[1]))
	MIR_insert_insn_before (context, func_item, insn,
				MIR_new_insn (context, MIR_MOV, insn->ops[0], insn->ops[1]));
      MIR_remove_insn (context, func_item, insn);
    } else if ((code == MIR_BT || code == MIR_BTS || code == MIR_BF || code == MIR_BFS)
	       && insn->ops[1].mode == MIR_OP_INT && (insn->ops[1].u.i == 0 || insn->ops[1].u.i == 1)) {
      if ((code == MIR_BT || code == MIR_BTS) == (insn->ops[1].u.i == 1))
	MIR_insert_insn_before (context, func_item, insn,
				MIR_new_insn (context, MIR_JMP, insn->ops[0]));
      MIR_remove_insn (context, func_item, insn);
      // ??? make imm always second,  what is about mem?
    } else {
      _MIR_simplify_insn (context, func_item, insn, mem_float_p);
    }
  }
  make_one_ret (context, func_item);
}

static void set_inline_reg_map (MIR_context_t context, MIR_reg_t old_reg, MIR_reg_t new_reg) {
  while (VARR_LENGTH (MIR_reg_t, inline_reg_map) <= old_reg)
    VARR_PUSH (MIR_reg_t, inline_reg_map, 0);
  VARR_SET (MIR_reg_t, inline_reg_map, old_reg, new_reg);
}

/* Only simplified code should be inlined because we need already
   extensions and one return.  */
void MIR_inline (MIR_context_t context, MIR_item_t func_item) {
  int alloca_p;
  size_t i, actual_nops, nargs, nvars;
  const char *name;
  MIR_type_t type, *res_types;
  MIR_var_t var;
  MIR_reg_t ret_reg, old_reg, new_reg, temp_reg;
  MIR_insn_t func_insn, next_func_insn, call, insn, new_insn, ret_label;
  MIR_item_t called_func_item;
  MIR_func_t func, called_func;
  char buff[50];
  
  mir_assert (func_item->item_type == MIR_func_item);
  func = func_item->u.func;
  for (func_insn = DLIST_HEAD (MIR_insn_t, func->insns);
       func_insn != NULL;
       func_insn = next_func_insn) {
    next_func_insn = DLIST_NEXT (MIR_insn_t, func_insn);
    if (func_insn->code != MIR_INLINE)
      continue;
    call = func_insn;
    mir_assert (call->ops[1].mode == MIR_OP_REF);
    called_func_item = call->ops[1].u.ref;
    while (called_func_item != NULL && called_func_item->item_type == MIR_import_item)
      called_func_item = called_func_item->ref_def;
    if (called_func_item == NULL || called_func_item->item_type != MIR_func_item)
      continue;
    called_func = called_func_item->u.func;
    if (called_func->vararg_p)
      continue;
    res_types = call->ops[0].u.ref->u.proto->res_types;
    ret_label = MIR_new_label (context);
    MIR_insert_insn_after (context, func_item, call, ret_label);
    func->n_inlines++;
    nargs = called_func->nargs;
    nvars = VARR_LENGTH (MIR_var_t, called_func->vars);
    for (i = 0; i < nvars; i++) {
      VARR_TRUNC (char, temp_string, 0);
      sprintf (buff, ".c%d_", func->n_inlines);
      VARR_PUSH_ARR (char, temp_string, buff, strlen (buff));
      var = VARR_GET (MIR_var_t, called_func->vars, i);
      type = var.type == MIR_T_F || var.type == MIR_T_D || var.type == MIR_T_LD ? var.type : MIR_T_I64;
      old_reg = MIR_reg (context, var.name, called_func);
      VARR_PUSH_ARR (char, temp_string, var.name, strlen (var.name) + 1);
      new_reg = MIR_new_func_reg (context, func, type, VARR_ADDR (char, temp_string));
      set_inline_reg_map (context, old_reg, new_reg);
      if (i < nargs) { /* Parameter passing */
	new_insn = MIR_new_insn (context, type == MIR_T_F ? MIR_FMOV : type == MIR_T_D ? MIR_DMOV
				 : type == MIR_T_LD ? MIR_LDMOV : MIR_MOV,
				 MIR_new_reg_op (context, new_reg), call->ops[i + 2 + called_func->nres]);
	MIR_insert_insn_before (context, func_item, ret_label, new_insn);
      }
    }
    /* ??? No frame only alloca */
    /* Add new insns: */
    ret_reg = 0;
    alloca_p = FALSE;
    for (insn = DLIST_HEAD (MIR_insn_t, called_func->insns);
	 insn != NULL;
	 insn = DLIST_NEXT (MIR_insn_t, insn)) {
      actual_nops = MIR_insn_nops (context, insn);
      new_insn = MIR_copy_insn (context, insn);
      mir_assert (insn->code != MIR_VA_ARG && insn->code != MIR_VA_START && insn->code != MIR_VA_END);
      if (insn->code == MIR_ALLOCA)
	alloca_p = TRUE;
      for (i = 0; i < actual_nops; i++)
	switch (new_insn->ops[i].mode) {
	case MIR_OP_REG:
	  new_insn->ops[i].u.reg = VARR_GET (MIR_reg_t, inline_reg_map, new_insn->ops[i].u.reg);
	  break;
	case MIR_OP_MEM:
	  if (insn->ops[i].u.mem.base != 0)
	    new_insn->ops[i].u.mem.base = VARR_GET (MIR_reg_t, inline_reg_map, new_insn->ops[i].u.mem.base);
	  if (insn->ops[i].u.mem.index != 0)
	    new_insn->ops[i].u.mem.index = VARR_GET (MIR_reg_t, inline_reg_map, new_insn->ops[i].u.mem.index);
	  break;
	default: /* do nothing */
	  break;
	}
      if (new_insn->code != MIR_RET) {
	MIR_insert_insn_before (context, func_item, ret_label, new_insn);
      } else {
	/* should be the last insn after simplification */
	mir_assert (DLIST_NEXT (MIR_insn_t, insn) == NULL
		    && call->ops[0].mode == MIR_OP_REF
		    && call->ops[0].u.ref->item_type == MIR_proto_item);
	free (new_insn);
	mir_assert (called_func->nres == actual_nops);
	for (i = 0; i < actual_nops; i++) {
	  mir_assert (new_insn->ops[i].mode == MIR_OP_REG);
	  ret_reg = new_insn->ops[i].u.reg;
	  new_insn = MIR_new_insn (context,
				   res_types[i] == MIR_T_F ? MIR_FMOV
				   : res_types[i] == MIR_T_D ? MIR_DMOV
				   : res_types[i] == MIR_T_LD ? MIR_LDMOV : MIR_MOV,
				   call->ops[i + 2], MIR_new_reg_op (context, ret_reg));
	  MIR_insert_insn_before (context, func_item, ret_label, new_insn);
	}
      }
    }
    if (alloca_p) {
      temp_reg = _MIR_new_temp_reg (context, MIR_T_I64, func);
      new_insn = MIR_new_insn (context, MIR_BSTART, MIR_new_reg_op (context, temp_reg));
      MIR_insert_insn_after (context, func_item, call, new_insn);
      new_insn = MIR_new_insn (context, MIR_BEND, MIR_new_reg_op (context, temp_reg));
      MIR_insert_insn_before (context, func_item, ret_label, new_insn);
    }
    MIR_remove_insn (context, func_item, call);
  }
}

const char *_MIR_uniq_string (MIR_context_t context, const char * str) {
  return string_store (context, &strings, &string_tab, str).str;
}

/* The next two function can be called any time relative to
   load/linkage.  You can also call them many times for the same name
   but you should always use the same prototype or/and addr for the
   same proto/func name.  */
MIR_item_t _MIR_builtin_proto (MIR_context_t context, MIR_module_t module, const char *name,
			       size_t nres, MIR_type_t *res_types, size_t nargs, ...) {
  size_t i;
  va_list argp;
  MIR_var_t arg;
  MIR_item_t proto_item;
  MIR_module_t saved_module = curr_module;
  
  va_start (argp, nargs);
  VARR_TRUNC (MIR_var_t, temp_vars, 0);
  for (i = 0; i < nargs; i++) {
    arg.type = va_arg (argp, MIR_type_t);
    arg.name = va_arg (argp, const char *);
    VARR_PUSH (MIR_var_t, temp_vars, arg);
  }
  va_end(argp);
  name = _MIR_uniq_string (context, name);
  proto_item = find_item (context, name, module);
  if (proto_item != NULL) {
    if (proto_item->item_type == MIR_proto_item
	&& proto_item->u.proto->nres == nres
	&& VARR_LENGTH (MIR_var_t, proto_item->u.proto->args) == nargs) {
      for (i = 0; i < nres; i++)
	if (res_types[i] != proto_item->u.proto->res_types[i])
	  break;
      if (i >= nres) {
	for (i = 0; i < nargs; i++)
	  if (VARR_GET (MIR_var_t, temp_vars, i).type
	      != VARR_GET (MIR_var_t, proto_item->u.proto->args, i).type)
	    break;
	if (i >= nargs)
	  return proto_item;
      }
    }
    (*error_func) (MIR_repeated_decl_error,
		   "_MIR_builtin_proto: proto item %s was already defined differently", name);
  }
  saved_module = curr_module;
  curr_module = module;
  proto_item = MIR_new_proto_arr (context, name, nres, res_types, nargs, VARR_ADDR (MIR_var_t, temp_vars));
  DLIST_REMOVE (MIR_item_t, curr_module->items, proto_item);
  DLIST_PREPEND (MIR_item_t, curr_module->items, proto_item); /* make it first in the list */
  curr_module = saved_module;
  return proto_item;
}

MIR_item_t _MIR_builtin_func (MIR_context_t context, MIR_module_t module, const char *name, void *addr) {
  MIR_item_t item, ref_item;
  MIR_module_t saved_module = curr_module;
  
  name = _MIR_uniq_string (context, name);
  if ((ref_item = find_item (context, name, &environment_module)) != NULL) {
    if (ref_item->item_type != MIR_import_item || ref_item->addr != addr)
      (*error_func) (MIR_repeated_decl_error,
		     "_MIR_builtin_func: func %s has already another address", name);
  } else {
    curr_module = &environment_module;
    /* Use import for builtin func: */
    item = new_export_import_forward (context, name, MIR_import_item, "import", TRUE);
    HTAB_DO (MIR_item_t, module_item_tab, item, HTAB_INSERT, ref_item);
    mir_assert (item == ref_item);
    DLIST_APPEND (MIR_item_t, environment_module.items, item);
    ref_item->addr = addr;
    curr_module = saved_module;
  }
  if ((item = find_item (context, name, module)) != NULL) {
    if (item->item_type != MIR_import_item || item->addr != addr || item->ref_def != ref_item)
      (*error_func) (MIR_repeated_decl_error,
		     "_MIR_builtin_func: func name %s was already defined differently in the module", name);
  } else {
    curr_module = module;
    item = new_export_import_forward (context, name, MIR_import_item, "import", FALSE);
    DLIST_REMOVE (MIR_item_t, curr_module->items, item);
    DLIST_PREPEND (MIR_item_t, curr_module->items, item); /* make it first in the list */
    item->addr = ref_item->addr;
    item->ref_def = ref_item;
    curr_module = saved_module;
  }
  return item;
}



#include <sys/mman.h>
#include <unistd.h>

struct code_holder {
  uint8_t *start, *free, *bound;
};

typedef struct code_holder code_holder_t;

DEF_VARR (code_holder_t);

struct machine_code_context {
  VARR (code_holder_t) *code_holders;
  size_t page_size;
};

#define code_holders context->machine_code_context->code_holders
#define page_size context->machine_code_context->page_size

uint8_t *_MIR_publish_code (MIR_context_t context, const uint8_t *code, size_t code_len) {
  uint8_t *start, *mem;
  size_t len;
  int new_p = TRUE;
  
  if ((len = VARR_LENGTH (code_holder_t, code_holders)) > 0) {
    code_holder_t *ch_ptr = VARR_ADDR (code_holder_t, code_holders) + len - 1;
    uint8_t *free_addr = (uint8_t *) ((uint64_t) (ch_ptr->free + 15) / 16 * 16); /* align */
    
    if (free_addr + code_len < ch_ptr->bound) {
      mem = free_addr;
      ch_ptr->free = free_addr + code_len;
      new_p = FALSE;
      start = ch_ptr->start;
      len = ch_ptr->bound - start;
    }
  }
  if (new_p) {
    code_holder_t ch;
    size_t npages = (code_len + page_size - 1) / page_size;
    
    len = page_size * npages;
    mem = (uint8_t *) mmap (NULL, len, PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
      return NULL;
    start = ch.start = mem;
    ch.free = mem + code_len;
    ch.bound = mem + len;
    VARR_PUSH (code_holder_t, code_holders, ch);
  }
  memcpy (mem, code, code_len);
  return mem;
}

void _MIR_update_code_arr (MIR_context_t context, uint8_t *base, size_t nloc, MIR_code_reloc_t relocs) {
  for (size_t i = 0; i < nloc; i++)
    memcpy (base + relocs[i].offset, &relocs[i].value, sizeof (void *));
}

void _MIR_update_code (MIR_context_t context, uint8_t *base, size_t nloc, ...) {
  va_list args;

  va_start (args, nloc);
  for (size_t i = 0; i < nloc; i++) {
    size_t offset = va_arg (args, size_t);
    void *value = va_arg (args, void *);

    memcpy (base + offset, &value, sizeof (void *));
  }
  va_end (args);
}

static void machine_init (MIR_context_t context);
static void machine_finish (MIR_context_t context);

static void code_init (MIR_context_t context) {
  if ((context->machine_code_context = malloc (sizeof (struct machine_code_context))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for context");
  page_size = sysconf(_SC_PAGE_SIZE);
  VARR_CREATE (code_holder_t, code_holders, 128);
  machine_init (context);
}

static void code_finish (MIR_context_t context) {
  while (VARR_LENGTH (code_holder_t, code_holders) != 0) {
    code_holder_t ch = VARR_POP (code_holder_t, code_holders);
    munmap (ch.start, ch.bound - ch.start);
  }
  VARR_DESTROY (code_holder_t, code_holders);
  machine_finish (context);
  free (context->machine_code_context); context->machine_code_context = NULL;
}



#if MIR_IO

/* Input/output of binary MIR.  Major goal of binary MIR is fast
   reading, not compression ratio.  Text MIR major CPU time consumer
   is a scanner.  Mostly in reading binary MIR we skip the scanner
   part by using tokens.  Each token starts with a tag which describes
   subsequent optional bytes.  */

typedef enum {
  TAG_U0, TAG_U1, TAG_U2, TAG_U3, TAG_U4, TAG_U5, TAG_U6, TAG_U7, TAG_U8,
  TAG_I1, TAG_I2, TAG_I3, TAG_I4, TAG_I5, TAG_I6, TAG_I7, TAG_I8,
  TAG_F, TAG_D, TAG_LD, /* 4, 8, 16 bytes for floating point numbers */
  TAG_REG1, TAG_REG2, TAG_REG3, TAG_REG4, /* Reg string number in 1, 2, 3, 4 bytes */
  TAG_NAME1, TAG_NAME2, TAG_NAME3, TAG_NAME4, /* Name string number in 1, 2, 3, 4 bytes */
  TAG_STR1, TAG_STR2, TAG_STR3, TAG_STR4, /* String number in 1, 2, 3, 4 bytes */
  TAG_LAB1, TAG_LAB2, TAG_LAB3, TAG_LAB4, /* Label number in 1, 2, 3, 4 bytes */
  /* Tags for memory operands.  The memory address parts are the subsequent tokens */
  TAG_MEM_DISP, TAG_MEM_BASE, TAG_MEM_INDEX, TAG_MEM_DISP_BASE, TAG_MEM_DISP_INDEX,
  TAG_MEM_BASE_INDEX, TAG_MEM_DISP_BASE_INDEX,
  /* MIR types. The same order as MIR types: */
  TAG_TI8, TAG_TU8, TAG_TI16, TAG_TU16, TAG_TI32, TAG_TU32, TAG_TI64, TAG_TU64,
  TAG_TF, TAG_TD, TAG_TP, TAG_TV, TAG_TBLOCK,
  TAG_EOI, TAG_EOF, /* end of insn with variable number operands (e.g. a call) or end of file */
  /* unsigned integer 0..127 is kept in one byte.  The most significant bit of the byte is 1: */
  U0_MASK = 0x7f, U0_FLAG = 0x80,
} bin_tag_t;

/* MIR binary format:

   VERSION
   NSTR
   (string)*
   ( ((label)* (insn code) (operand)* | STRN=(func|local|import|export|forward|<data>) ...) EOI? )*
   EOF

   where
   o VERSION and NSTR are unsigned tokens
   o insn code is unsigned token
   o string is string number tokens
   o operand is unsigned, signed, float, double, string, label, memory tokens
   o EOI, EOF - tokens for end of insn (optional for most insns) and end of file
*/

static const int CURR_BIN_VERSION = 1;

typedef char *char_ptr_t;
DEF_VARR (char_ptr_t);

DEF_VARR (uint64_t);

struct io_context {
  VARR (string_t) *output_strings;
  HTAB (string_t) *output_string_tab;
  VARR (char_ptr_t) *bin_strings;
  VARR (uint64_t) *insn_label_string_nums;
};

#define output_strings context->io_context->output_strings
#define output_string_tab context->io_context->output_string_tab
#define bin_strings context->io_context->bin_strings
#define insn_label_string_nums context->io_context->insn_label_string_nums
  
static void put_byte (FILE *f, int ch) {
  if (f == NULL)
    return;
  fputc (ch, f);
}

static int uint_length (uint64_t u) {
  int n;
  
  if (u <= 127)
    return 0;
  for (n = 0; u != 0; n++)
    u >>= CHAR_BIT;
  return n;
}

static void put_uint (FILE *f, uint64_t u, int nb) {
  if (f == NULL)
    return;
  for (int n = 0; n < nb; n++) {
    put_byte (f, u & 0xff);
    u >>= CHAR_BIT;
  }
}

static int int_length (int64_t i) {
  int n = 0;
  
  do {
    n++;
    i /= (1 << CHAR_BIT);
  } while (i != 0);
  return n;
}

static void put_int (FILE *f, int64_t u, int nb) {
  if (f == NULL)
    return;
  for (int n = 0; n < nb; n++) {
    put_byte (f, u & 0xff);
    u >>= CHAR_BIT;
  }
}

static void put_float (FILE *f, float fl) {
  union {uint32_t u; float f;} u;

  if (f == NULL)
    return;
  u.f = fl;
  put_uint (f, u.u, sizeof (uint32_t));
}

static void put_double (FILE *f, double d) {
  union {uint64_t u; double d;} u;

  if (f == NULL)
    return;
  u.d = d;
  put_uint (f, u.d, sizeof (uint64_t));
}

static void put_ldouble (FILE *f, long double ld) {
  union {uint64_t u[2]; long double ld;} u;

  if (f == NULL)
    return;
  u.ld = ld;
  put_uint (f, u.u[0], sizeof (uint64_t));
  put_uint (f, u.u[1], sizeof (uint64_t));
}

/* Write binary MIR */

static void write_int (FILE *f, int64_t i) {
  int nb;

  if (f == NULL)
    return;
  nb = int_length (i);
  put_byte (f, TAG_I1 + nb - 1);
  put_int (f, i, nb);
}

static void write_uint (FILE *f, uint64_t u) {
  int nb;

  if (f == NULL)
    return;
  if ((nb = uint_length (u)) == 0) {
    put_byte (f, 0x80 | u);
    return;
  }
  put_byte (f, TAG_U1 + nb - 1);
  put_uint (f, u, nb);
}

static void write_float (FILE *f, float fl) {
  if (f == NULL)
    return;
  put_byte (f, TAG_F);
  put_float (f, fl);
}

static void write_double (FILE *f, double d) {
  if (f == NULL)
    return;
  put_byte (f, TAG_D);
  put_double (f, d);
}

static void write_ldouble (FILE *f, long double ld) {
  if (f == NULL)
    return;
  put_byte (f, TAG_LD);
  put_ldouble (f, ld);
}

static void write_str_tag (MIR_context_t context, FILE *f, const char *str, bin_tag_t start_tag) {
  int nb, ok_p;
  string_t string;
  
  if (f == NULL) {
    string_store (context, &output_strings, &output_string_tab, str);
    return;
  }
  ok_p = string_find (&output_strings, &output_string_tab, str, &string);
  mir_assert (ok_p && string.num >= 1);
  nb = uint_length (string.num - 1);
  mir_assert (nb <= 4);
  if (nb == 0)
    nb = 1;
  put_byte (f, start_tag + nb - 1);
  put_uint (f, string.num - 1, nb);
}

static void write_str (MIR_context_t context, FILE *f, const char *str) { write_str_tag (context, f, str, TAG_STR1); }
static void write_name (MIR_context_t context, FILE *f, const char *str) { write_str_tag (context, f, str, TAG_NAME1); }
static void write_reg (MIR_context_t context, FILE *f, const char *reg_name) { write_str_tag (context, f, reg_name, TAG_REG1); }

static void write_type (FILE *f, MIR_type_t t) { put_byte (f, TAG_TI8 + (t - MIR_T_I8)); }

static void write_lab (FILE *f, MIR_label_t lab) {
  int nb;
  uint64_t lab_num;
  
  if (f == NULL)
    return;
  lab_num = lab->ops[0].u.u;
  nb = uint_length (lab_num);
  mir_assert (nb <= 4);
  if (nb == 0)
    nb = 1;
  put_byte (f, TAG_LAB1 + nb - 1);
  put_uint (f, lab_num, nb);
}

static void write_op (MIR_context_t context, FILE *f, MIR_func_t func, MIR_op_t op) {
  switch (op.mode) {
  case MIR_OP_REG:
    write_reg (context, f, MIR_reg_name (context, op.u.reg, func));
    break;
  case MIR_OP_INT:
    write_int (f, op.u.i);
    break;
  case MIR_OP_UINT:
    write_uint (f, op.u.u);
    break;
  case MIR_OP_FLOAT:
    write_float (f, op.u.f);
    break;
  case MIR_OP_DOUBLE:
    write_double (f, op.u.d);
    break;
  case MIR_OP_LDOUBLE:
    write_ldouble (f, op.u.ld);
    break;
  case MIR_OP_MEM: {
    bin_tag_t tag;
    
    if (op.u.mem.disp != 0) {
      if (op.u.mem.base != 0)
	tag = op.u.mem.index != 0 ? TAG_MEM_DISP_BASE_INDEX : TAG_MEM_DISP_BASE;
      else
	tag = op.u.mem.index != 0 ? TAG_MEM_DISP_INDEX : TAG_MEM_DISP;
    } else if (op.u.mem.base != 0) {
      tag = op.u.mem.index != 0 ? TAG_MEM_BASE_INDEX : TAG_MEM_BASE;
    } else if (op.u.mem.index != 0) {
      tag = TAG_MEM_INDEX;
    }
    put_byte (f, tag);
    write_type (f, op.u.mem.type);
    if (op.u.mem.disp != 0)
      write_int (f, op.u.mem.disp);
    if (op.u.mem.base != 0)
      write_reg (context, f, MIR_reg_name (context, op.u.mem.base, func));
    if (op.u.mem.index != 0) {
      write_reg (context, f, MIR_reg_name (context, op.u.mem.index, func));
      write_uint (f, op.u.mem.scale);
    }
    break;
  }
  case MIR_OP_REF:
    write_name (context, f, MIR_item_name (context, op.u.ref));
    break;
  case MIR_OP_STR:
    write_str (context, f, op.u.str);
    break;
  case MIR_OP_LABEL:
    write_lab (f, op.u.label);
    break;
  default: /* ??? HARD_REG, HARD_REG_MEM */
    mir_assert (FALSE);
  }
}

static void write_insn (MIR_context_t context, FILE *f, MIR_func_t func, MIR_insn_t insn) {
  size_t i, nops;
  MIR_insn_code_t code = insn->code;

  if (code == MIR_LABEL) {
    write_lab (f, insn);
    return;
  }
  nops = MIR_insn_nops (context, insn);
  write_uint (f, code);
  for (i = 0; i < nops; i++) {
    write_op (context, f, func, insn->ops[i]);
  }
  if (insn_descs[code].op_modes[0] == MIR_OP_BOUND) {
    /* first operand mode is undefined if it is a variable operand insn */
    mir_assert (MIR_call_code_p (code) || code == MIR_RET);
    put_byte (f, TAG_EOI);
  }
}

static void write_item (MIR_context_t context, FILE *f, MIR_item_t item) {
  MIR_insn_t insn;
  MIR_func_t func;
  MIR_proto_t proto;
  MIR_var_t var;
  size_t i, nlocals;

  if (item->item_type == MIR_import_item) {
    write_name (context, f, "import");
    write_name (context, f, item->u.import);
    return;
  }
  if (item->item_type == MIR_export_item) {
    write_name (context, f, "export");
    write_name (context, f, item->u.export);
    return;
  }
  if (item->item_type == MIR_forward_item) {
    write_name (context, f, "forward");
    write_name (context, f, item->u.forward);
    return;
  }
  if (item->item_type == MIR_bss_item) {
    if (item->u.bss->name == NULL) {
      write_name (context, f, "bss");
    } else {
      write_name (context, f, "nbss");
      write_name (context, f, item->u.bss->name);
    }
    write_uint (f, item->u.bss->len);
    return;
  }
  if (item->item_type == MIR_ref_data_item) {
    if (item->u.ref_data->name == NULL) {
      write_name (context, f, "ref");
    } else {
      write_name (context, f, "nref");
      write_name (context, f, item->u.ref_data->name);
    }
    write_name (context, f, MIR_item_name (context, item->u.ref_data->ref_item));
    return;
  }
  if (item->item_type == MIR_expr_data_item) {
    if (item->u.expr_data->name == NULL) {
      write_name (context, f, "expr");
    } else {
      write_name (context, f, "nexpr");
      write_name (context, f, item->u.expr_data->name);
    }
    write_name (context, f, MIR_item_name (context, item->u.expr_data->expr_item));
    return;
  }
  if (item->item_type == MIR_data_item) {
    MIR_data_t data = item->u.data;
    
    if (data->name == NULL) {
      write_name (context, f, "data");
    } else {
      write_name (context, f, "ndata");
      write_name (context, f, data->name);
    }
    write_type (f, data->el_type);
    for (i = 0; i < data->nel; i++)
      switch (data->el_type) {
      case MIR_T_I8: write_int (f, ((int8_t *) data->u.els)[i]); break;
      case MIR_T_U8: write_uint (f, ((uint8_t *) data->u.els)[i]); break;
      case MIR_T_I16: write_int (f, ((int16_t *) data->u.els)[i]); break;
      case MIR_T_U16: write_uint (f, ((uint16_t *) data->u.els)[i]); break;
      case MIR_T_I32: write_int (f, ((int32_t *) data->u.els)[i]); break;
      case MIR_T_U32: write_uint (f, ((uint32_t *) data->u.els)[i]); break;
      case MIR_T_I64: write_int (f, ((int64_t *) data->u.els)[i]); break;
      case MIR_T_U64: write_uint (f, ((uint64_t *) data->u.els)[i]); break;
      case MIR_T_F: write_float (f, ((float *) data->u.els)[i]); break;
      case MIR_T_D: write_double (f, ((double *) data->u.els)[i]); break;
      case MIR_T_LD: write_ldouble (f, ((long double *) data->u.els)[i]); break;
	/* only ptr as ref ??? */
      case MIR_T_P: write_uint (f, ((uintptr_t *) data->u.els)[i]); break;
      default: mir_assert (FALSE);
      }
    put_byte (f, TAG_EOI);
    return;
  }
  if (item->item_type == MIR_proto_item) {
    proto = item->u.proto;
    write_name (context, f, "proto");
    write_name (context, f, proto->name);
    write_uint (f, proto->vararg_p != 0);
    write_uint (f, proto->nres);
    for (i = 0; i < proto->nres; i++)
      write_type (f, proto->res_types[i]);
    for (i = 0; i < VARR_LENGTH (MIR_var_t, proto->args); i++) {
      var = VARR_GET (MIR_var_t, proto->args, i);
      write_type (f, var.type);
      write_name (context, f, var.name);
    }
    put_byte (f, TAG_EOI);
    return;
  }
  func = item->u.func;
  write_name (context, f, "func");
  write_name (context, f, func->name);
  write_uint (f, func->vararg_p != 0);
  write_uint (f, func->nres);
  for (i = 0; i < func->nres; i++)
    write_type (f, func->res_types[i]);
  for (i = 0; i < func->nargs; i++) {
    var = VARR_GET (MIR_var_t, func->vars, i);
    write_type (f, var.type);
    write_name (context, f, var.name);
  }
  put_byte (f, TAG_EOI);
  nlocals = VARR_LENGTH (MIR_var_t, func->vars) - func->nargs;
  if (nlocals !=  0) {
    write_name (context, f, "local");
    for (i = 0; i < nlocals; i++) {
      var = VARR_GET (MIR_var_t, func->vars, i + func->nargs);
      write_type (f, var.type);
      write_name (context, f, var.name);
    }
    put_byte (f, TAG_EOI);
  }
  for (insn = DLIST_HEAD (MIR_insn_t, func->insns); insn != NULL; insn = DLIST_NEXT (MIR_insn_t, insn))
    write_insn (context, f, func, insn);
  write_name (context, f, "endfunc");
}

static void write_module (MIR_context_t context, FILE *f, MIR_module_t module) {
  write_name (context, f, "module");
  write_name (context, f, module->name);
  for (MIR_item_t item = DLIST_HEAD (MIR_item_t, module->items);
       item != NULL;
       item = DLIST_NEXT (MIR_item_t, item))
    write_item (context, f, item);
  write_name (context, f, "endmodule");
}

static void write_modules (MIR_context_t context, FILE *f) {
  for (MIR_module_t module = DLIST_HEAD (MIR_module_t, all_modules);
       module != NULL;
       module = DLIST_NEXT (MIR_module_t, module))
    write_module (context, f, module);
}

void MIR_write (MIR_context_t context, FILE *f) {
  string_init (&output_strings, &output_string_tab);
  write_modules (context, NULL); /* store strings */
  write_uint (f, CURR_BIN_VERSION);
  write_uint (f, VARR_LENGTH (string_t, output_strings) - 1);
  for (size_t i = 1; i < VARR_LENGTH (string_t, output_strings); i++) { /* output strings */
    fputs (VARR_GET (string_t, output_strings, i).str, f);
    fputc ('\0', f);
  }
  write_modules (context, f);
  put_byte (f, TAG_EOF);
  string_finish (&output_strings, &output_string_tab);
}



static int get_byte (MIR_context_t context, FILE *f) {
  int c = fgetc (f);

  if (c == EOF)
    (*error_func) (MIR_binary_io_error, "unfinished binary MIR");
  return c;
}

typedef union {
  uint64_t u;
  int64_t i;
  float f;
  double d;
  long double ld;
  MIR_type_t t;
  MIR_reg_t reg;
} token_attr_t;

static uint64_t get_uint (MIR_context_t context, FILE *f, int nb) {
  uint64_t res = 0;
  
  for (int i = 0; i < nb; i++)
    res |= (unsigned char) get_byte (context, f) << (i * CHAR_BIT);
  return res;
}

static int64_t get_int (MIR_context_t context, FILE *f, int nb) {
  int sh = (8 - nb) * CHAR_BIT;
  
  mir_assert (0 < nb && nb <= 8);
  return (int64_t) (get_uint (context, f, nb) << sh) >> sh;
}

static float get_float (MIR_context_t context, FILE *f) {
  union {uint32_t u; float f;} u;

  u.u = get_uint (context, f, sizeof (uint32_t));
  return u.f;
}

static double get_double (MIR_context_t context, FILE *f) {
  union {uint64_t u; double d;} u;

  u.u = get_uint (context, f, sizeof (uint64_t));
  return u.d;
}

static long double get_ldouble (MIR_context_t context, FILE *f) {
  union {uint64_t u[2]; long double ld;} u;

  u.u[0] = get_uint (context, f, sizeof (uint64_t));
  u.u[1] = get_uint (context, f, sizeof (uint64_t));
  return u.ld;
}

static const char *to_str (MIR_context_t context, uint64_t str_num) {
  if (str_num >= VARR_LENGTH (char_ptr_t, bin_strings))
    (*error_func) (MIR_binary_io_error, "wrong string num %lu", str_num);
  return VARR_GET (char_ptr_t, bin_strings, str_num);
}

static MIR_reg_t to_reg (MIR_context_t context, uint64_t reg_str_num, MIR_item_t func) {
  return MIR_reg (context, to_str (context, reg_str_num), func->u.func);
}

static MIR_label_t to_lab (MIR_context_t context, uint64_t lab_num) {
  return create_label (context, lab_num);
}

static void read_all_strings (MIR_context_t context, FILE *f, uint64_t nstr) {
  int c;
  char *str;

  VARR_TRUNC (char_ptr_t, bin_strings, 0);
  for (uint64_t i = 0; i < nstr; i++) {
    VARR_TRUNC (char, temp_string, 0);
    do {
      c = get_byte (context, f);
      VARR_PUSH (char, temp_string, c);
    } while (c != '\0');
    str = malloc (VARR_LENGTH (char, temp_string));
    strcpy (str, VARR_ADDR (char, temp_string));
    VARR_PUSH (char_ptr_t, bin_strings, str);
  }
}

static uint64_t read_uint (MIR_context_t context, FILE *f, const char *err_msg) {
  int c = get_byte (context, f);
  
  if (c & U0_FLAG)
    return c & U0_MASK;
  if (TAG_U1 > c || c > TAG_U8)
    (*error_func) (MIR_binary_io_error, err_msg);
  return get_uint (context, f, c - TAG_U1 + 1);
}

static MIR_type_t tag_type (bin_tag_t tag) { return (MIR_type_t) (tag - TAG_TI8) + MIR_T_I8; }

static MIR_type_t read_type (MIR_context_t context, FILE *f, const char *err_msg) {
  int c = get_byte (context, f);
  
  if (TAG_TI8 > c || c > TAG_TBLOCK)
    (*error_func) (MIR_binary_io_error, err_msg);
  return tag_type (c);
}

static const char *read_name (MIR_context_t context, FILE *f, const char *err_msg) {
  int c = get_byte (context, f);
  
  if (TAG_NAME1 > c || c > TAG_NAME4)
    (*error_func) (MIR_binary_io_error, err_msg);
  return to_str (context, get_uint (context, f, c - TAG_NAME1 + 1));
}

static bin_tag_t read_token (MIR_context_t context, FILE *f, token_attr_t *attr) {
  int c = get_byte (context, f);
  
  if (c & U0_FLAG) {
    attr->u = c & U0_MASK;
    return TAG_U0;
  }
  switch (c) {
  case TAG_U1: case TAG_U2: case TAG_U3: case TAG_U4: case TAG_U5: case TAG_U6: case TAG_U7: case TAG_U8:
    attr->u = get_uint (context, f, c - TAG_U1 + 1); break;
  case TAG_I1: case TAG_I2: case TAG_I3: case TAG_I4: case TAG_I5: case TAG_I6: case TAG_I7: case TAG_I8:
    attr->i = get_int (context, f, c - TAG_I1 + 1); break;
  case TAG_F: attr->f = get_float (context, f); break;
  case TAG_D: attr->d = get_double (context, f); break;
  case TAG_LD: attr->ld = get_ldouble (context, f); break;
  case TAG_REG1: case TAG_REG2: case TAG_REG3: case TAG_REG4:
    attr->u = get_uint (context, f, c - TAG_REG1 + 1); break;
  case TAG_NAME1: case TAG_NAME2: case TAG_NAME3: case TAG_NAME4:
    attr->u = get_uint (context, f, c - TAG_NAME1 + 1); break;
  case TAG_STR1: case TAG_STR2: case TAG_STR3: case TAG_STR4:
    attr->u = get_uint (context, f, c - TAG_STR1 + 1); break;
  case TAG_LAB1: case TAG_LAB2: case TAG_LAB3: case TAG_LAB4:
    attr->u = get_uint (context, f, c - TAG_LAB1 + 1); break;
  case TAG_MEM_DISP: case TAG_MEM_BASE: case TAG_MEM_INDEX: case TAG_MEM_DISP_BASE: case TAG_MEM_DISP_INDEX:
  case TAG_MEM_BASE_INDEX: case TAG_MEM_DISP_BASE_INDEX: case TAG_EOI: case TAG_EOF:
    break;
  case TAG_TI8: case TAG_TU8: case TAG_TI16: case TAG_TU16: case TAG_TI32: case TAG_TU32:
  case TAG_TI64: case TAG_TU64: case TAG_TF: case TAG_TD: case TAG_TP: case TAG_TV: case TAG_TBLOCK:
    attr->t = (MIR_type_t) (c - TAG_TI8) + MIR_T_I8;
    break;
  default:
    (*error_func) (MIR_binary_io_error, "wrong tag %d", c);
  }
  return c;
}

static MIR_disp_t read_disp (MIR_context_t context, FILE *f) {
  bin_tag_t tag;
  token_attr_t attr;

  tag = read_token (context, f, &attr);
  if (TAG_I1 > tag || tag > TAG_I8)
    (*error_func) (MIR_binary_io_error, "memory disp has wrong tag %d", tag);
  return attr.i;
}

static MIR_reg_t read_reg (MIR_context_t context, FILE *f, MIR_item_t func) {
  bin_tag_t tag;
  token_attr_t attr;

  tag = read_token (context, f, &attr);
  if (TAG_REG1 > tag || tag > TAG_REG4)
    (*error_func) (MIR_binary_io_error, "register has wrong tag %d", tag);
  return to_reg (context, attr.u, func);
}

static int read_operand (MIR_context_t context, FILE *f, MIR_op_t *op, MIR_item_t func) {
  bin_tag_t tag;
  token_attr_t attr;
  MIR_type_t t;
  MIR_disp_t disp;
  MIR_reg_t base, index;
  MIR_scale_t scale;
  
  tag = read_token (context, f, &attr);
  switch (tag) {
  case TAG_U0: case TAG_U1: case TAG_U2: case TAG_U3: case TAG_U4:
  case TAG_U5: case TAG_U6: case TAG_U7: case TAG_U8:
    *op = MIR_new_uint_op (context, attr.u); break;
  case TAG_I1: case TAG_I2: case TAG_I3: case TAG_I4: case TAG_I5: case TAG_I6: case TAG_I7: case TAG_I8:
    *op = MIR_new_int_op (context, attr.i); break;
  case TAG_F: *op = MIR_new_float_op (context, attr.f); break;
  case TAG_D: *op = MIR_new_double_op (context, attr.d); break;
  case TAG_LD: *op = MIR_new_ldouble_op (context, attr.ld); break;
  case TAG_REG1: case TAG_REG2: case TAG_REG3: case TAG_REG4:
    *op = MIR_new_reg_op (context, to_reg (context, attr.u, func)); break;
  case TAG_NAME1: case TAG_NAME2: case TAG_NAME3: case TAG_NAME4: {
    const char *name = to_str (context, attr.u);
    MIR_item_t item = find_item (context, name, func->module);

    if (item == NULL)
      (*error_func) (MIR_binary_io_error, "not found item %s", name);
    *op = MIR_new_ref_op (context, item);
    break;
  }
  case TAG_STR1: case TAG_STR2: case TAG_STR3: case TAG_STR4:
    *op = MIR_new_str_op (context, to_str (context, attr.u)); break;
  case TAG_LAB1: case TAG_LAB2: case TAG_LAB3: case TAG_LAB4:
    *op = MIR_new_label_op (context, to_lab (context, attr.u)); break;
  case TAG_MEM_DISP: case TAG_MEM_BASE: case TAG_MEM_INDEX: case TAG_MEM_DISP_BASE: case TAG_MEM_DISP_INDEX:
  case TAG_MEM_BASE_INDEX: case TAG_MEM_DISP_BASE_INDEX:
    t = read_type (context, f, "wrong memory type");
    disp = (tag == TAG_MEM_DISP || tag == TAG_MEM_DISP_BASE || tag == TAG_MEM_DISP_INDEX
	    || tag == TAG_MEM_DISP_BASE_INDEX ? read_disp (context, f) : 0);
    base = (tag == TAG_MEM_BASE || tag == TAG_MEM_DISP_BASE || tag == TAG_MEM_BASE_INDEX
	    || tag == TAG_MEM_DISP_BASE_INDEX ? read_reg (context, f, func) : 0);
    index = 0; scale = 0;
    if (tag == TAG_MEM_INDEX || tag == TAG_MEM_DISP_INDEX || tag == TAG_MEM_BASE_INDEX
	|| tag == TAG_MEM_DISP_BASE_INDEX) {
      index = read_reg (context, f, func);
      scale = read_uint (context, f, "wrong memory index scale");
    }
    *op = MIR_new_mem_op (context, t, disp, base, index, scale);
    break;
  case TAG_EOI:
    return FALSE;
  default:
    mir_assert (FALSE);
  }
  return TRUE;
}

static int func_proto_read (MIR_context_t context, FILE *f, uint64_t *nres_ptr) {
  bin_tag_t tag;
  token_attr_t attr;
  MIR_var_t var;
  int vararg_p = read_uint (context, f, "wrong vararg flag") != 0;
  uint64_t i, nres = read_uint (context, f, "wrong func nres");
  
  VARR_TRUNC (MIR_type_t, temp_types, 0);
  for (i = 0; i < nres; i++) {
    tag = read_token (context, f, &attr);
    if (TAG_TI8 > tag || tag > TAG_TBLOCK)
      (*error_func) (MIR_binary_io_error, "wrong prototype result type tag %d", tag);
    VARR_PUSH (MIR_type_t, temp_types, tag_type (tag));
  }
  VARR_TRUNC (MIR_var_t, temp_vars, 0);
  for (;;) {
    tag = read_token (context, f, &attr);
    if (tag == TAG_EOI)
      break;
    if (TAG_TI8 > tag || tag > TAG_TBLOCK)
      (*error_func) (MIR_binary_io_error, "wrong prototype arg type tag %d", tag);
    var.type = tag_type (tag);
    var.name = read_name (context, f, "wrong arg name");
    VARR_PUSH (MIR_var_t, temp_vars, var);
  }
  *nres_ptr = nres;
  return vararg_p;
}

void MIR_read (MIR_context_t context, FILE *f) {
  int version;
  bin_tag_t tag;
  token_attr_t attr;
  MIR_label_t lab;
  uint64_t nstr, nres, u;
  MIR_op_t op;
  size_t n, nop;
  const char *name, *item_name;
  MIR_module_t module;
  MIR_item_t func, item;
  
  version = read_uint (context, f, "wrong header");
  if (version > CURR_BIN_VERSION)
    (*error_func) (MIR_binary_io_error, "can not read version %d MIR binary: expected %d or less",
		   version, CURR_BIN_VERSION);
  nstr = read_uint (context, f, "wrong header");
  read_all_strings (context, f, nstr);
  module = NULL;
  func = NULL;
  for (;;) {
    VARR_TRUNC (uint64_t, insn_label_string_nums, 0);
    tag = read_token (context, f, &attr);
    while (TAG_LAB1 <= tag && tag <= TAG_LAB4) {
      VARR_PUSH (uint64_t, insn_label_string_nums, attr.u);
      tag = read_token (context, f, &attr);
    }
    VARR_TRUNC (MIR_op_t, temp_insn_ops, 0);
    if (TAG_NAME1 <= tag && tag <= TAG_NAME4) {
      name = to_str (context, attr.u);
      if (strcmp (name, "module") == 0) {
	name = read_name (context, f, "wrong module name");
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "insn label before module %s", name);
	if (module != NULL)
	  (*error_func) (MIR_binary_io_error, "nested module %s", name);
	module = MIR_new_module (context, name);
      } else if (strcmp (name, "endmodule") == 0) {
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "endmodule should have no labels");
	if (module == NULL)
	  (*error_func) (MIR_binary_io_error, "endmodule without module");
	MIR_finish_module (context);
	module = NULL;
      } else if (strcmp (name, "proto") == 0) {
	name = read_name (context, f, "wrong prototype name");
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "insn label before proto %s", name);
	if (module == NULL)
	  (*error_func) (MIR_binary_io_error, "prototype %s outside module", name);
	if (func_proto_read (context, f, &nres))
	  MIR_new_vararg_proto_arr (context, name, nres, VARR_ADDR (MIR_type_t, temp_types),
				    VARR_LENGTH (MIR_var_t, temp_vars), VARR_ADDR (MIR_var_t, temp_vars));
	else
	  MIR_new_proto_arr (context, name, nres, VARR_ADDR (MIR_type_t, temp_types),
			     VARR_LENGTH (MIR_var_t, temp_vars), VARR_ADDR (MIR_var_t, temp_vars));
      } else if (strcmp (name, "func") == 0) {
	name = read_name (context, f, "wrong func name");
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "insn label before func %s", name);
	if (func != NULL)
	  (*error_func) (MIR_binary_io_error, "nested func %s", name);
	if (module == NULL)
	  (*error_func) (MIR_binary_io_error, "func %s outside module", name);
	if (func_proto_read (context, f, &nres))
	  func = MIR_new_vararg_func_arr (context, name, nres, VARR_ADDR (MIR_type_t, temp_types),
					  VARR_LENGTH (MIR_var_t, temp_vars),
					  VARR_ADDR (MIR_var_t, temp_vars));
	else
	  func = MIR_new_func_arr (context, name, nres, VARR_ADDR (MIR_type_t, temp_types),
				   VARR_LENGTH (MIR_var_t, temp_vars), VARR_ADDR (MIR_var_t, temp_vars));
      } else if (strcmp (name, "endfunc") == 0) {
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "endfunc should have no labels");
	if (func == NULL)
	  (*error_func) (MIR_binary_io_error, "endfunc without func");
	MIR_finish_func (context);
	func = NULL;
      } else if (strcmp (name, "export") == 0) {
	name = read_name (context, f, "wrong export name");
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "export %s should have no labels", name);
	MIR_new_export (context, name);
      } else if (strcmp (name, "import") == 0) {
	name = read_name (context, f, "wrong import name");
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "import %s should have no labels", name);
	MIR_new_import (context, name);
      } else if (strcmp (name, "forward") == 0) {
	name = read_name (context, f, "wrong forward name");
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "forward %s should have no labels", name);
	MIR_new_forward (context, name);
      } else if (strcmp (name, "nbss") == 0 || strcmp (name, "bss") == 0) {
	name = strcmp (name, "nbss") == 0 ? read_name (context, f, "wrong bss name") : NULL;
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "bss %s should have no labels", name == NULL ? "" : name);
	u = read_uint (context, f, "wrong bss len");
	MIR_new_bss (context, name, u);
      } else if (strcmp (name, "nref") == 0 || strcmp (name, "ref") == 0) {
	name = strcmp (name, "nref") == 0 ? read_name (context, f, "wrong ref data name") : NULL;
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "ref data %s should have no labels", name == NULL ? "" : name);
	item_name = read_name (context, f, "wrong ref data item name");
	if ((item = find_item (context, item_name, module)) == NULL)
	  (*error_func) (MIR_binary_io_error, "ref data refers to non-existing item %s", item_name);
	MIR_new_ref_data (context, name, item);
      } else if (strcmp (name, "nexpr") == 0 || strcmp (name, "expr") == 0) {
	name = strcmp (name, "nexpr") == 0 ? read_name (context, f, "wrong expr name") : NULL;
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "expr %s should have no labels", name == NULL ? "" : name);
	item_name = read_name (context, f, "wrong expr func name");
	if ((item = find_item (context, item_name, module)) == NULL
	    || item->item_type != MIR_func_item)
	  (*error_func) (MIR_binary_io_error, "expr refers to non-function %s", item_name);
	MIR_new_expr_data (context, name, item);
      } else if (strcmp (name, "ndata") == 0 || strcmp (name, "data") == 0) {
	MIR_type_t type;
	size_t nel;
	union {
	  uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64;
	  int8_t i8; int16_t i16; int32_t i32; int64_t i64;
	} v;
	
	name = strcmp (name, "ndata") == 0 ? read_name (context, f, "wrong data name") : NULL;
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "data %s should have no labels", name == NULL ? "" : name);
	tag = read_token (context, f, &attr);
	if (TAG_TI8 > tag || tag > TAG_TBLOCK)
	  (*error_func) (MIR_binary_io_error, "wrong data type tag %d", tag);
	type = tag_type (tag);
	VARR_TRUNC (uint8_t, temp_data, 0);
	for (nel = 0;; nel++) {
	  tag = read_token (context, f, &attr);
	  if (tag == TAG_EOI)
	    break;
	  switch (tag) {
	  case TAG_U0: case TAG_U1: case TAG_U2: case TAG_U3: case TAG_U4:
	  case TAG_U5: case TAG_U6: case TAG_U7: case TAG_U8:
	    switch (type) {
	    case MIR_T_U8: v.u8 = attr.u; push_data (context, &v.u8, sizeof (uint8_t)); break;
	    case MIR_T_U16: v.u16 = attr.u; push_data (context, (uint8_t *) &v.u16, sizeof (uint16_t)); break;
	    case MIR_T_U32: v.u32 = attr.u; push_data (context, (uint8_t *) &v.u32, sizeof (uint32_t)); break;
	    case MIR_T_U64: v.u64 = attr.u; push_data (context, (uint8_t *) &v.i64, sizeof (uint64_t)); break;
	    default:
	      (*error_func) (MIR_binary_io_error, "data type %s does not correspond value type",
			     type_str (type));
	    }
	    break;
	  case TAG_I1: case TAG_I2: case TAG_I3: case TAG_I4:
	  case TAG_I5: case TAG_I6: case TAG_I7: case TAG_I8:
	    switch (type) {
	    case MIR_T_I8: v.i8 = attr.i; push_data (context, (uint8_t *) &v.i8, sizeof (int8_t)); break;
	    case MIR_T_I16: v.i16 = attr.i; push_data (context, (uint8_t *) &v.i16, sizeof (int16_t)); break;
	    case MIR_T_I32: v.i32 = attr.i; push_data (context, (uint8_t *) &v.i32, sizeof (int32_t)); break;
	    case MIR_T_I64: v.i64 = attr.i; push_data (context, (uint8_t *) &v.i64, sizeof (int64_t)); break;
	    default:
	      (*error_func) (MIR_binary_io_error, "data type %s does not correspond value type",
			     type_str (type));
	    }
	    break;
	  case TAG_F:
	    if (type != MIR_T_F)
	      (*error_func) (MIR_binary_io_error, "data type %s does not correspond value type",
			     type_str (type));
	    push_data (context, (uint8_t *) &attr.f, sizeof (float));
	    break;
	  case TAG_D:
	    if (type != MIR_T_D)
	      (*error_func) (MIR_binary_io_error, "data type %s does not correspond value type",
			     type_str (type));
	    push_data (context, (uint8_t *) &attr.d, sizeof (double));
	    break;
	  case TAG_LD:
	    if (type != MIR_T_LD)
	      (*error_func) (MIR_binary_io_error, "data type %s does not correspond value type",
			     type_str (type));
	    push_data (context, (uint8_t *) &attr.ld, sizeof (long double));
	    break;
	    /* ??? ptr */
	  default:
	    (*error_func) (MIR_binary_io_error, "wrong data value tag %d", tag);
	  }
	}
	MIR_new_data (context, name, type, VARR_LENGTH (uint8_t, temp_data), VARR_ADDR (uint8_t, temp_data));
      } else if (strcmp (name, "local") == 0) {
	if (func == NULL)
	  (*error_func) (MIR_binary_io_error, "local outside func");
	if (VARR_LENGTH (uint64_t, insn_label_string_nums) != 0)
	  (*error_func) (MIR_binary_io_error, "local should have no labels");
	for (;;) {
	  tag = read_token (context, f, &attr);
	  if (tag == TAG_EOI)
	    break;
	  if (TAG_TI8 > tag || tag > TAG_TBLOCK)
	    (*error_func) (MIR_binary_io_error, "wrong local var type tag %d", tag);
	  MIR_new_func_reg (context, func->u.func, tag_type (tag), read_name (context, f, "wrong local var name"));
	}
      } else {
	(*error_func) (MIR_binary_io_error, "unknown insn name %s", name);
      }
    } else if (TAG_U0 <= tag && tag <= TAG_U8) { /* insn code */
      MIR_insn_code_t insn_code = attr.u;
      
      if (insn_code >= MIR_LABEL)
	(*error_func) (MIR_binary_io_error, "wrong insn code %d", insn_code);
      for (uint64_t i = 0; i < VARR_LENGTH (uint64_t, insn_label_string_nums); i++) {
	lab = to_lab (context, VARR_GET (uint64_t, insn_label_string_nums, i));
	MIR_append_insn (context, func, lab);
      }
      nop = insn_code_nops (context, insn_code);
      mir_assert (nop != 0 || MIR_call_code_p (insn_code) || insn_code == MIR_RET);
      for (n = 0; (nop == 0 || n < nop) && read_operand (context, f, &op, func); n++)
	VARR_PUSH (MIR_op_t, temp_insn_ops, op);
      if (nop != 0 && n < nop)
	(*error_func) (MIR_binary_io_error, "wrong number of operands of insn %s", insn_name (insn_code));
      MIR_append_insn (context, func, MIR_new_insn_arr (context, insn_code, n, VARR_ADDR (MIR_op_t, temp_insn_ops)));
    } else if (tag == TAG_EOF) {
      break;
    } else {
      (*error_func) (MIR_binary_io_error, "wrong token %d", tag);
    }
  }
  if (func != NULL)
    (*error_func) (MIR_binary_io_error, "unfinished func %s", func->u.func->name);
  if (module != NULL)
    (*error_func) (MIR_binary_io_error, "unfinished module %s", module->name);
  if (fgetc (f) != EOF)
    (*error_func) (MIR_binary_io_error, "garbage at the end of file");
}

static void io_init (MIR_context_t context) {
  if ((context->io_context = malloc (sizeof (struct io_context))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for context");
  VARR_CREATE (char_ptr_t, bin_strings, 512);
  VARR_CREATE (uint64_t, insn_label_string_nums, 64);
}

static void io_finish (MIR_context_t context) {
  VARR_DESTROY (uint64_t, insn_label_string_nums);
  VARR_DESTROY (char_ptr_t, bin_strings);
  free (context->io_context); context->io_context = NULL;
}

#endif /* if MIR_IO */



/* Reading MIR text file */

#if MIR_SCAN

#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>

typedef struct insn_name {
  const char *name;
  MIR_insn_code_t code;
} insn_name_t;

static int insn_name_eq (insn_name_t in1, insn_name_t in2) { return strcmp (in1.name, in2.name) == 0; }
static htab_hash_t insn_name_hash (insn_name_t in) { return mir_hash (in.name, strlen (in.name), 0); }

enum token_code { TC_INT, TC_FLOAT, TC_DOUBLE, TC_LDOUBLE, TC_NAME, TC_STR,
		  TC_NL, TC_EOF, TC_LEFT_PAR, TC_RIGHT_PAR, TC_COMMA, TC_SEMICOL, TC_COL };

typedef struct token {
  enum token_code code;
  union {
    int64_t i;
    float f;
    double d;
    long double ld;
    const char *name;
    const char *str;
  } u;
} token_t;

DEF_HTAB (insn_name_t);
typedef const char *label_name_t;
DEF_VARR (label_name_t);

typedef struct label_desc {
  const char *name;
  MIR_label_t label;
} label_desc_t;

DEF_HTAB (label_desc_t);

struct scan_context {
  jmp_buf error_jmp_buf;
  size_t curr_lno;
  HTAB (insn_name_t) *insn_name_tab;
  const char *input_string;
  size_t input_string_char_num;
  VARR (label_name_t) *label_names;
  HTAB (label_desc_t) *label_desc_tab;
};

#define error_jmp_buf context->scan_context->error_jmp_buf
#define curr_lno context->scan_context->curr_lno
#define insn_name_tab context->scan_context->insn_name_tab
#define input_string context->scan_context->input_string
#define input_string_char_num context->scan_context->input_string_char_num
#define label_names context->scan_context->label_names
#define label_desc_tab context->scan_context->label_desc_tab

static void MIR_NO_RETURN process_error (MIR_context_t context, enum MIR_error_type error_type, const char *message) {
  (*error_func) (error_type, "ln %lu: %s", (unsigned long) curr_lno, message);
  longjmp (error_jmp_buf, TRUE);
}

/* Read number using GET_CHAR and UNGET_CHAR and already read
   character CH.  It should be guaranted that the input has a righ
   prefix (+|-)?[0-9].  Return base, float and double flag through
   BASE, FLOAT_P, DOUBLE_P.  Put number representation (0x or 0X
   prefix is removed) into TEMP_STRING.  */
static void scan_number (MIR_context_t context, int ch,
			 int get_char (MIR_context_t), void unget_char (MIR_context_t, int),
			 int *base, int *float_p, int *double_p, int *ldouble_p) {
  enum scan_number_code {NUMBER_OK, ABSENT_EXPONENT, NON_DECIMAL_FLOAT, WRONG_OCTAL_INT} err_code = NUMBER_OK;
  int dec_p, hex_p, hex_char_p;
  
  *base = 10;
  *ldouble_p = *double_p = *float_p = FALSE;
  if (ch == '+' || ch == '-') {
    VARR_PUSH (char, temp_string, ch);
    ch = get_char (context);
  }
  mir_assert ('0' <= ch && ch <= '9');
  if (ch == '0') {
    ch = get_char (context);
    if (ch != 'x' && ch != 'X') {
      *base = 8;
      unget_char (context, ch);
      ch = '0';
    } else {
      ch = get_char (context);
      *base = 16;
    }
  }
  dec_p = hex_p = FALSE;
  for (;;) {
    if (ch != '_')
      VARR_PUSH (char, temp_string, ch);
    ch = get_char (context);
    if (ch == '8' || ch == '9')
      dec_p = TRUE;
    hex_char_p = (('a' <= ch && ch <= 'f') || ('A' <= ch && ch <= 'F'));
    if (ch != '_' && ! isdigit (ch) && (*base != 16 || ! hex_char_p))
      break;
    if (hex_char_p)
      hex_p = TRUE;
  }
  mir_assert (*base == 16 || ! hex_p);
  if (ch == '.') {
    *double_p = TRUE;
    do {
      if (ch != '_')
	VARR_PUSH (char, temp_string, ch);
      ch = get_char (context);
    } while (isdigit (ch) || ch == '_');
  }
  if (ch == 'e' || ch == 'E') {
    *double_p = TRUE;
    ch = get_char (context);
    if (ch != '+' && ch != '-' && ! isdigit (ch))
      err_code = ABSENT_EXPONENT;
    else {
      VARR_PUSH (char, temp_string, 'e');
      if (ch == '+' || ch == '-') {
	VARR_PUSH (char, temp_string, ch);
	ch = get_char (context);
	if (! isdigit (ch))
	  err_code = ABSENT_EXPONENT;
      }
      if (err_code == NUMBER_OK)
	do {
	  if (ch != '_')
	    VARR_PUSH (char, temp_string, ch);
	  ch = get_char (context);
	} while (isdigit (ch) || ch == '_');
    }
  }
  if (*double_p) {
    if (*base == 16)
      err_code = NON_DECIMAL_FLOAT;
    else if (ch == 'f' || ch == 'F') {
      *float_p = TRUE; *double_p = FALSE;
      ch = get_char (context);
    } else if (ch == 'l' || ch == 'L') {
      *ldouble_p = TRUE; *double_p = FALSE;
      ch = get_char (context);
    }
  } else if (*base == 8 && dec_p)
    err_code = WRONG_OCTAL_INT;
  VARR_PUSH (char, temp_string, '\0');
  unget_char (context, ch);
}

static void scan_string (MIR_context_t context, token_t *t, int c,
			 int get_char (MIR_context_t), void unget_char (MIR_context_t, int)) {
  int ch_code;

  mir_assert (c == '\"');
  VARR_TRUNC (char, temp_string, 0);
  for (;;) {
    if ((c = get_char (context)) == EOF || c == '\n')
      process_error (context, MIR_syntax_error, "unfinished string");
    if (c == '"')
      break;
    if (c == '\\') {
      if ((c = get_char (context)) == 'n') c = '\n';
      else if (c == 't') c = '\t';
      else if (c == 'v') c = '\v';
      else if (c == 'a') c = '\a';
      else if (c == 'b') c = '\b';
      else if (c == 'r') c = '\r';
      else if (c == 'f') c = '\f';
      else if (c == '\\' || c == '\'' || c == '\"')
	;
      else if (c == '\n') {
	curr_lno++;
	continue;
      } else if (isdigit (c) && c != '8' && c != '9') {
	ch_code = c - '0';
	c = get_char (context);
	if (! isdigit (c) || c == '8' || c == '9')
	  unget_char (context, c);
	else {
	  ch_code = ch_code * 8 + c - '0';
	  c = get_char (context);
	  if (! isdigit (c) || c == '8' || c == '9')
	    unget_char (context, c);
	  else
	    ch_code = ch_code * 8 + c - '0';
	}
	c = ch_code;
      } else if (c == 'x') {
	/* Hex escape code.  */
	ch_code = 0;
	for (int i = 2; i > 0; i--) {
	  c = get_char (context);
	  if (! isxdigit (c))
	    process_error (context, MIR_syntax_error, "wrong hexadecimal escape");
	  c = '0' <= c && c <= '9' ? c - '0' : 'a' <= c && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
	  ch_code = (ch_code << 4) | c;
	}
	c = ch_code;
      }
    }
    VARR_PUSH (char, temp_string, c);
  }
  VARR_PUSH (char, temp_string, 0);
  t->code = TC_STR;
  t->u.str = string_store (context, &strings, &string_tab, VARR_ADDR (char, temp_string)).str;
}

static int get_string_char (MIR_context_t context) {
  int ch = input_string[input_string_char_num];

  if (ch == '\0')
    return EOF;
  input_string_char_num++;
  if (ch == '\n')
    curr_lno++;
  return ch;
}

static void unget_string_char (MIR_context_t context, int ch) {
  if (input_string_char_num == 0 || ch == EOF)
    return;
  input_string_char_num--;
  mir_assert (input_string[input_string_char_num] == ch);
  if (ch == '\n')
    curr_lno--;
}

static void scan_token (MIR_context_t context, token_t *token,
			int (*get_char) (MIR_context_t), void (*unget_char) (MIR_context_t, int)) {
  int ch;
  
  for (;;) {
    ch = get_char (context);
    switch (ch) {
    case EOF:
      token->code = TC_EOF;
      return;
    case ' ': case '\t':
      break;
    case '#':
      while ((ch = get_char (context)) != '\n' && ch != EOF)
	;
      /* Fall through: */
    case '\n':
      curr_lno++;
      token->code = TC_NL;
      return;
    case '(':
      token->code = TC_LEFT_PAR;
      return;
    case ')':
      token->code = TC_RIGHT_PAR;
      return;
    case ',':
      token->code = TC_COMMA;
      return;
    case ';':
      token->code = TC_SEMICOL;
      return;
    case ':':
      token->code = TC_COL;
      return;
    case '"':
      scan_string (context, token, ch, get_char, unget_char);
      return;
    default:
      VARR_TRUNC (char, temp_string, 0);
      if (isalpha (ch) || ch == '_' || ch == '$' || ch == '%' || ch == '.') {
	do {
	  VARR_PUSH (char, temp_string, ch);
	  ch = get_char (context);
	} while (isalpha (ch) || isdigit (ch) || ch == '_' || ch == '$' || ch == '%' || ch == '.');
	VARR_PUSH (char, temp_string, '\0');
	unget_char (context, ch);
	token->u.str = _MIR_uniq_string (context, VARR_ADDR (char, temp_string));
	token->code = TC_NAME;
	return;
      } else if (ch == '+' || ch == '-' || isdigit (ch)) {
	const char *repr;
	char *end;
	int next_ch, base, float_p, double_p, ldouble_p;
	
	if (ch == '+' || ch == '-') {
	  next_ch = get_char (context);
	  if (! isdigit (next_ch))
	    process_error (context, MIR_syntax_error, "no number after a sign");
	  unget_char (context, next_ch);
	}
	scan_number (context, ch, get_char, unget_char, &base, &float_p, &double_p, &ldouble_p);
	repr = VARR_ADDR (char, temp_string);
	errno = 0;
	if (float_p) {
	  token->code = TC_FLOAT;
	  token->u.f = strtof (repr, &end);
	} else if (double_p) {
	  token->code = TC_DOUBLE;
	  token->u.d = strtod (repr, &end);
	} else if (ldouble_p) {
	  token->code = TC_LDOUBLE;
	  token->u.ld = strtold (repr, &end);
	} else {
	  token->code = TC_INT;
	  token->u.i = sizeof (long) == sizeof (int64_t) ? strtol (repr, &end, base) : strtoll (repr, &end, base);
	}
	mir_assert (*end == '\0');
	if (errno != 0)
	  ;
	return;
      } else {
	process_error (context, MIR_syntax_error, "wrong char");
      }
    }
  }
}

static int label_eq (label_desc_t l1, label_desc_t l2) { return strcmp (l1.name, l2.name) == 0; }
static htab_hash_t label_hash (label_desc_t l) { return mir_hash (l.name, strlen (l.name), 0); }

static MIR_label_t create_label_desc (MIR_context_t context, const char *name) {
  MIR_label_t label;
  label_desc_t label_desc;

  label_desc.name = name;
  if (HTAB_DO (label_desc_t, label_desc_tab, label_desc, HTAB_FIND, label_desc)) {
    label = label_desc.label;
  } else {
    label_desc.label = label = MIR_new_label (context);
    HTAB_DO (label_desc_t, label_desc_tab, label_desc, HTAB_INSERT, label_desc);
  }
  return label;
}

MIR_type_t MIR_str2type (MIR_context_t context, const char *type_name) {
  if (strcmp (type_name, "i64") == 0)
    return MIR_T_I64;
  if (strcmp (type_name, "u64") == 0)
    return MIR_T_U64;
  if (strcmp (type_name, "f") == 0)
    return MIR_T_F;
  if (strcmp (type_name, "d") == 0)
    return MIR_T_D;
  if (strcmp (type_name, "ld") == 0)
    return MIR_T_LD;
  if (strcmp (type_name, "p") == 0)
    return MIR_T_P;
  if (strcmp (type_name, "i32") == 0)
    return MIR_T_I32;
  if (strcmp (type_name, "u32") == 0)
    return MIR_T_U32;
  if (strcmp (type_name, "i16") == 0)
    return MIR_T_I16;
  if (strcmp (type_name, "u16") == 0)
    return MIR_T_U16;
  if (strcmp (type_name, "i8") == 0)
    return MIR_T_I8;
  if (strcmp (type_name, "u8") == 0)
    return MIR_T_U8;
  return MIR_T_BOUND;
}

static void read_func_proto (MIR_context_t context, size_t nops, MIR_op_t *ops) {
  MIR_var_t var;
  size_t i;
  
  VARR_TRUNC (MIR_type_t, temp_types, 0);
  for (i = 0; i < nops; i++) {
    var.name = (const char *) ops[i].u.mem.disp;
    if ((var.name = (const char *) ops[i].u.mem.disp) != NULL)
      break;
    var.type = ops[i].u.mem.type;
    VARR_PUSH (MIR_type_t, temp_types, var.type);
  }
  VARR_TRUNC (MIR_var_t, temp_vars, 0);
  for (; i < nops; i++) {
    if (ops[i].mode != MIR_OP_MEM)
      process_error (context, MIR_syntax_error, "wrong prototype/func arg");
    var.type = ops[i].u.mem.type;
    var.name = (const char *) ops[i].u.mem.disp;
    if (var.name == NULL)
      process_error (context, MIR_syntax_error, "all func/prototype args should have type:name form");
    VARR_PUSH (MIR_var_t, temp_vars, var);
  }
}

/* Syntax:
     program: { insn / sep }
     sep : ';' | NL
     insn : {label ':'}* [ code [ {op / ','} ] ]
     label : name
     code : name
     op : name | int | float | double | long double | mem | str
     mem : type ':' addr
     addr : disp
          | [ disp ] '(' sib ')'
     sib : name | [ name ] ',' name [ ',' scale]
     disp : int | name
     scale : int

*/

void MIR_scan_string (MIR_context_t context, const char *str) {
  token_t t;
  const char *name;
  MIR_module_t module = NULL;
  MIR_item_t item, func = NULL;
  MIR_insn_code_t insn_code;
  MIR_insn_t insn;
  MIR_type_t type, data_type = MIR_T_BOUND;
  MIR_op_t op, *op_addr;
  MIR_label_t label;
  size_t n;
  int64_t i, nargs;
  int module_p, end_module_p, proto_p, func_p, end_func_p, dots_p, export_p, import_p, forward_p;
  int bss_p, ref_p, expr_p, string_p, local_p, push_op_p, read_p, disp_p;
  insn_name_t in, el;

  curr_lno = 1;
  input_string = str;
  input_string_char_num = 0;
  t.code = TC_NL;
  for (;;) {
    if (setjmp (error_jmp_buf)) {
      while (t.code != TC_NL && t.code != EOF)
	scan_token (context, &t, get_string_char, unget_string_char);
      if (t.code == TC_EOF)
	break;
    }
    VARR_TRUNC (label_name_t, label_names, 0);
    scan_token (context, &t, get_string_char, unget_string_char);
    while (t.code == TC_NL)
      scan_token (context, &t, get_string_char, unget_string_char);
    if (t.code == TC_EOF)
      break;
    for (;;) { /* label_names */
      if (t.code != TC_NAME)
	process_error (context, MIR_syntax_error, "insn should start with label or insn name");
      name = t.u.name;
      scan_token (context, &t, get_string_char, unget_string_char);
      if (t.code != TC_COL)
	break;
      VARR_PUSH (label_name_t, label_names, name);
      scan_token (context, &t, get_string_char, unget_string_char);
      if (t.code == TC_NL)
	scan_token (context, &t, get_string_char, unget_string_char); /* label_names without insn */
    }
    module_p = end_module_p = proto_p = func_p = end_func_p = FALSE;
    export_p = import_p = forward_p = bss_p = ref_p = expr_p = string_p = local_p = FALSE;
    if (strcmp (name, "module") == 0) {
      module_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 1)
	process_error (context, MIR_syntax_error, "only one label should be used for module");
    } else if (strcmp (name, "endmodule") == 0) {
      end_module_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
	process_error (context, MIR_syntax_error, "endmodule should have no labels");
    } else if (strcmp (name, "proto") == 0) {
      proto_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 1)
	process_error (context, MIR_syntax_error, "only one label should be used for proto");
    } else if (strcmp (name, "func") == 0) {
      func_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 1)
	process_error (context, MIR_syntax_error, "only one label should be used for func");
    } else if (strcmp (name, "endfunc") == 0) {
      end_func_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
	process_error (context, MIR_syntax_error, "endfunc should have no labels");
    } else if (strcmp (name, "export") == 0) {
      export_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
	process_error (context, MIR_syntax_error, "export should have no labels");
    } else if (strcmp (name, "import") == 0) {
      import_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
	process_error (context, MIR_syntax_error, "import should have no labels");
    } else if (strcmp (name, "forward") == 0) {
      forward_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) != 0)
	process_error (context, MIR_syntax_error, "forward should have no labels");
    } else if (strcmp (name, "bss") == 0) {
      bss_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
	process_error (context, MIR_syntax_error, "at most one label should be used for bss");
    } else if (strcmp (name, "ref") == 0) {
      ref_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
	process_error (context, MIR_syntax_error, "at most one label should be used for ref");
    } else if (strcmp (name, "expr") == 0) {
      expr_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
	process_error (context, MIR_syntax_error, "at most one label should be used for expr");
    } else if ((data_type = MIR_str2type (context, name)) != MIR_T_BOUND) {
      if (VARR_LENGTH (label_name_t, label_names) > 1)
	process_error (context, MIR_syntax_error, "at most one label should be used for data");
    } else if (strcmp (name, "string") == 0) {
      string_p = TRUE;
      if (VARR_LENGTH (label_name_t, label_names) > 1)
	process_error (context, MIR_syntax_error, "at most one label should be used for string");
    } else if (strcmp (name, "local") == 0) {
      local_p = TRUE;
      if (func == NULL)
	process_error (context, MIR_syntax_error, "local outside func");
      if (VARR_LENGTH (label_name_t, label_names) != 0)
	process_error (context, MIR_syntax_error, "local should have no labels");
    } else {
      in.name = name;
      if (! HTAB_DO (insn_name_t, insn_name_tab, in, HTAB_FIND, el))
	process_error (context, MIR_syntax_error, "Unknown insn");
      insn_code = el.code;
      for (n = 0; n < VARR_LENGTH (label_name_t, label_names); n++) {
	label = create_label_desc (context, VARR_GET (label_name_t, label_names, n));
	if (func != NULL)
	  MIR_append_insn (context, func, label);
      }
    }
    VARR_TRUNC (MIR_op_t, temp_insn_ops, 0);
    dots_p = FALSE;
    for (;;) { /* ops */
      if (t.code == TC_NL || t.code == TC_SEMICOL) {
	/* insn end */
	break;
      }
      push_op_p = read_p = TRUE;
      switch (t.code) {
      case TC_NAME: {
	name = t.u.name;
	scan_token (context, &t, get_string_char, unget_string_char);
	if ((func_p || proto_p) && strcmp (name, "...") == 0) {
	  dots_p = TRUE;
	  break;
	}
	read_p = FALSE;
	if (t.code != TC_COL && ! proto_p && ! func_p && ! local_p) {
	  if (export_p) {
	    MIR_new_export (context, name);
	    push_op_p = FALSE;
	  } else if (import_p) {
	    MIR_new_import (context, name);
	    push_op_p = FALSE;
	  } else if (forward_p) {
	    MIR_new_forward (context, name);
	    push_op_p = FALSE;
	  } else if (! module_p && ! end_module_p && ! proto_p
		     && ! func_p && ! end_func_p && ! local_p
		     && MIR_branch_code_p (insn_code)
		     && VARR_LENGTH (MIR_op_t, temp_insn_ops) == 0) {
	    op = MIR_new_label_op (context, create_label_desc (context, name));
	  } else if (! expr_p && ! ref_p && func_reg_p (context, func->u.func, name)) {
	    op.mode = MIR_OP_REG;
	    op.u.reg = MIR_reg (context, name, func->u.func);
	  } else if ((item = find_item (context, name, module)) != NULL) {
	    op = MIR_new_ref_op (context, item);
	  } else {
	    process_error (context, MIR_syntax_error, "undeclared name");
	  }
	  break;
	}
	/* Memory, type only, arg, or var */
	type = MIR_str2type (context, name);
	if (type == MIR_T_BOUND)
	  process_error (context, MIR_syntax_error, "Unknown type");
	else if (local_p && type != MIR_T_I64 && type != MIR_T_F && type != MIR_T_D && type != MIR_T_LD)
	  process_error (context, MIR_syntax_error, "wrong type for local var");
	op = MIR_new_mem_op (context, type, 0, 0, 0, 1);
	if (proto_p || func_p || local_p) {
	  if (t.code == TC_COL) {
	    scan_token (context, &t, get_string_char, unget_string_char);
	    if (t.code != TC_NAME)
	      process_error (context, MIR_syntax_error, func_p ? "wrong arg" : "wrong local var");
	    op.u.mem.disp = (MIR_disp_t) t.u.name;
	    scan_token (context, &t, get_string_char, unget_string_char);
	  }
	} else {
	  scan_token (context, &t, get_string_char, unget_string_char);
	  disp_p = FALSE;
	  if (t.code == TC_INT) {
	    op.u.mem.disp = t.u.i;
	    scan_token (context, &t, get_string_char, unget_string_char);
	    disp_p = TRUE;
	  } else if (t.code == TC_NAME) {
	    op.u.mem.disp = (MIR_disp_t) t.u.name;
	    scan_token (context, &t, get_string_char, unget_string_char);
	    disp_p = TRUE;
	  }
	  if (t.code == TC_LEFT_PAR) {
	    scan_token (context, &t, get_string_char, unget_string_char);
	    if (t.code == TC_NAME) {
	      op.u.mem.base = MIR_reg (context, t.u.name, func->u.func);
	      scan_token (context, &t, get_string_char, unget_string_char);
	    }
	    if (t.code == TC_COMMA) {
	      scan_token (context, &t, get_string_char, unget_string_char);
	      if (t.code != TC_NAME)
		process_error (context, MIR_syntax_error, "wrong index");
	      op.u.mem.index = MIR_reg (context, t.u.name, func->u.func);
	      scan_token (context, &t, get_string_char, unget_string_char);
	      if (t.code == TC_COMMA) {
		scan_token (context, &t, get_string_char, unget_string_char);
		if (t.code != TC_INT)
		  process_error (context, MIR_syntax_error, "wrong scale");
		if (t.u.i != 1 && t.u.i != 2 && t.u.i != 4 && t.u.i != 8)
		  process_error (context, MIR_syntax_error, "scale is not 1, 2, 4, or 8");
		op.u.mem.scale = t.u.i;
		scan_token (context, &t, get_string_char, unget_string_char);
	      }
	    }
	    if (t.code != TC_RIGHT_PAR)
	      process_error (context, MIR_syntax_error, "wrong memory op");
	    scan_token (context, &t, get_string_char, unget_string_char);
	  } else if (! disp_p)
	    process_error (context, MIR_syntax_error, "wrong memory");
	}
	break;
      }
      case TC_INT:
	op.mode = MIR_OP_INT;
	op.u.i = t.u.i;
	break;
      case TC_FLOAT:
	op.mode = MIR_OP_FLOAT;
	op.u.f = t.u.f;
	break;
      case TC_DOUBLE:
	op.mode = MIR_OP_DOUBLE;
	op.u.d = t.u.d;
	break;
      case TC_LDOUBLE:
	op.mode = MIR_OP_LDOUBLE;
	op.u.ld = t.u.ld;
	break;
      case TC_STR:
	op.mode = MIR_OP_STR;
	op.u.str = t.u.str;
	break;
      default:
	break;
      }
      if (dots_p)
	break;
      if (push_op_p)
	VARR_PUSH (MIR_op_t, temp_insn_ops, op);
      if (read_p)
	scan_token (context, &t, get_string_char, unget_string_char);
      if (t.code != TC_COMMA)
	break;
      scan_token (context, &t, get_string_char, unget_string_char);
    }
    if (t.code != TC_NL && t.code != TC_EOF && t.code != TC_SEMICOL)
      process_error (context, MIR_syntax_error, "wrong insn end");
    if (module_p) {
      if (module != NULL)
	process_error (context, MIR_syntax_error, "nested module");
      if (VARR_LENGTH (MIR_op_t, temp_insn_ops) != 0)
	process_error (context, MIR_syntax_error, "module should have no params");
      module = MIR_new_module (context, VARR_GET (label_name_t, label_names, 0));
    } else if (end_module_p) {
      if (module == NULL)
	process_error (context, MIR_syntax_error, "standalone endmodule");
      if (VARR_LENGTH (MIR_op_t, temp_insn_ops) != 0)
	process_error (context, MIR_syntax_error, "endmodule should have no params");
      MIR_finish_module (context);
      module = NULL;
    } else if (bss_p) {
      if (VARR_LENGTH (MIR_op_t, temp_insn_ops) != 1)
	process_error (context, MIR_syntax_error, "bss should have one operand");
      op_addr = VARR_ADDR (MIR_op_t, temp_insn_ops);
      if (op_addr[0].mode != MIR_OP_INT || op_addr[0].u.i < 0)
	process_error (context, MIR_syntax_error, "wrong bss operand type or value");
      name = VARR_LENGTH (label_name_t, label_names) == 0 ? NULL : VARR_GET (label_name_t, label_names, 0);
      MIR_new_bss (context, name, op_addr[0].u.i);
    } else if (ref_p) {
      if (VARR_LENGTH (MIR_op_t, temp_insn_ops) != 1)
	process_error (context, MIR_syntax_error, "ref should have one operand");
      op_addr = VARR_ADDR (MIR_op_t, temp_insn_ops);
      if (op_addr[0].mode != MIR_OP_REF)
	process_error (context, MIR_syntax_error, "wrong ref operand");
      name = VARR_LENGTH (label_name_t, label_names) == 0 ? NULL : VARR_GET (label_name_t, label_names, 0);
      MIR_new_ref_data (context, name, op_addr[0].u.ref);
    } else if (expr_p) {
      if (VARR_LENGTH (MIR_op_t, temp_insn_ops) != 1)
	process_error (context, MIR_syntax_error, "expr should have one operand");
      op_addr = VARR_ADDR (MIR_op_t, temp_insn_ops);
      if (op_addr[0].mode != MIR_OP_REF || op_addr[0].u.ref->item_type != MIR_func_item)
	process_error (context, MIR_syntax_error, "wrong expr operand");
      name = VARR_LENGTH (label_name_t, label_names) == 0 ? NULL : VARR_GET (label_name_t, label_names, 0);
      MIR_new_expr_data (context, name, op_addr[0].u.ref);
    } else if (string_p) {
      if (VARR_LENGTH (MIR_op_t, temp_insn_ops) != 1)
	process_error (context, MIR_syntax_error, "string should have one operand");
      op_addr = VARR_ADDR (MIR_op_t, temp_insn_ops);
      if (op_addr[0].mode != MIR_OP_STR)
	process_error (context, MIR_syntax_error, "wrong string data operand type");
      name = VARR_LENGTH (label_name_t, label_names) == 0 ? NULL : VARR_GET (label_name_t, label_names, 0);
      MIR_new_string_data (context, name, op_addr[0].u.str);
    } else if (data_type != MIR_T_BOUND) {
      union {
	uint8_t u8; uint16_t u16; uint32_t u32; uint64_t u64;
	int8_t i8; int16_t i16; int32_t i32; int64_t i64;
      } v;
      
      n = VARR_LENGTH (MIR_op_t, temp_insn_ops);
      op_addr = VARR_ADDR (MIR_op_t, temp_insn_ops);
      VARR_TRUNC (uint8_t, temp_data, 0);
      for (i = 0; i < n; i++) {
	if (op_addr[i].mode != type2mode (data_type))
	  process_error (context, MIR_syntax_error, "data operand is not of data type");
	switch (data_type) {
	case MIR_T_I8: v.i8 = op_addr[i].u.i; push_data (context, (uint8_t *) &v.i8, sizeof (int8_t)); break;
	case MIR_T_U8: v.u8 = op_addr[i].u.u; push_data (context, (uint8_t *) &v.u8, sizeof (uint8_t)); break;
	case MIR_T_I16: v.i16 = op_addr[i].u.i; push_data (context, (uint8_t *) &v.i16, sizeof (int16_t)); break;
	case MIR_T_U16: v.u16 = op_addr[i].u.u; push_data (context, (uint8_t *) &v.u16, sizeof (uint16_t)); break;
	case MIR_T_I32: v.i32 = op_addr[i].u.i; push_data (context, (uint8_t *) &v.i32, sizeof (int32_t)); break;
	case MIR_T_U32: v.u32 = op_addr[i].u.u; push_data (context, (uint8_t *) &v.u32, sizeof (uint32_t)); break;
	case MIR_T_I64: v.i64 = op_addr[i].u.i; push_data (context, (uint8_t *) &v.i64, sizeof (int64_t)); break;
	case MIR_T_U64: v.u64 = op_addr[i].u.u; push_data (context, (uint8_t *) &v.u64, sizeof (uint64_t)); break;
	case MIR_T_F: push_data (context, (uint8_t *) &op_addr[i].u.f, sizeof (float)); break;
	case MIR_T_D: push_data (context, (uint8_t *) &op_addr[i].u.d, sizeof (double)); break;
	case MIR_T_LD: push_data (context, (uint8_t *) &op_addr[i].u.ld, sizeof (long double)); break;
	  /* ptr ??? */
	default:
	  process_error (context, MIR_syntax_error, "wrong data clause");
	}
      }
      name = VARR_LENGTH (label_name_t, label_names) == 0 ? NULL : VARR_GET (label_name_t, label_names, 0);
      MIR_new_data (context, name, data_type, VARR_LENGTH (uint8_t, temp_data), VARR_ADDR (uint8_t, temp_data));
    } else if (proto_p) {
      if (module == NULL)
	process_error (context, MIR_syntax_error, "prototype outside module");
      read_func_proto (context, VARR_LENGTH (MIR_op_t, temp_insn_ops), VARR_ADDR (MIR_op_t, temp_insn_ops));
      if (dots_p)
	MIR_new_vararg_proto_arr (context, VARR_GET (label_name_t, label_names, 0),
				  VARR_LENGTH (MIR_type_t, temp_types), VARR_ADDR (MIR_type_t, temp_types),
				  VARR_LENGTH (MIR_var_t, temp_vars), VARR_ADDR (MIR_var_t, temp_vars));
      else
	MIR_new_proto_arr (context, VARR_GET (label_name_t, label_names, 0),
			   VARR_LENGTH (MIR_type_t, temp_types), VARR_ADDR (MIR_type_t, temp_types),
			   VARR_LENGTH (MIR_var_t, temp_vars), VARR_ADDR (MIR_var_t, temp_vars));
    } else if (func_p) {
      if (module == NULL)
	process_error (context, MIR_syntax_error, "func outside module");
      if (func != NULL)
	process_error (context, MIR_syntax_error, "nested func");
      read_func_proto (context, VARR_LENGTH (MIR_op_t, temp_insn_ops), VARR_ADDR (MIR_op_t, temp_insn_ops));
      if (dots_p)
	func = MIR_new_vararg_func_arr (context, VARR_GET (label_name_t, label_names, 0),
					VARR_LENGTH (MIR_type_t, temp_types),
					VARR_ADDR (MIR_type_t, temp_types),
					VARR_LENGTH (MIR_var_t, temp_vars), VARR_ADDR (MIR_var_t, temp_vars));
      else
	func = MIR_new_func_arr (context, VARR_GET (label_name_t, label_names, 0),
				 VARR_LENGTH (MIR_type_t, temp_types), VARR_ADDR (MIR_type_t, temp_types),
				 VARR_LENGTH (MIR_var_t, temp_vars), VARR_ADDR (MIR_var_t, temp_vars));
      HTAB_CLEAR (label_desc_t, label_desc_tab, NULL);
    } else if (end_func_p) {
      if (func == NULL)
	process_error (context, MIR_syntax_error, "standalone endfunc");
      if (VARR_LENGTH (MIR_op_t, temp_insn_ops) != 0)
	process_error (context, MIR_syntax_error, "endfunc should have no params");
      func = NULL;
      MIR_finish_func (context);
    } else if (export_p || import_p || forward_p) { /* we already created items, now do nothing: */
      mir_assert (VARR_LENGTH (MIR_op_t, temp_insn_ops) == 0);
    } else if (local_p) {
      op_addr = VARR_ADDR (MIR_op_t, temp_insn_ops);
      n = VARR_LENGTH (MIR_op_t, temp_insn_ops);
      for (i = 0; i < n; i++) {
	if (op_addr[i].mode != MIR_OP_MEM || (const char *) op_addr[i].u.mem.disp == NULL)
	  process_error (context, MIR_syntax_error, "wrong local var");
	MIR_new_func_reg (context, func->u.func, op_addr[i].u.mem.type, (const char *) op_addr[i].u.mem.disp);
      }
    } else {
      insn = MIR_new_insn_arr (context, insn_code, VARR_LENGTH (MIR_op_t, temp_insn_ops),
			       VARR_ADDR (MIR_op_t, temp_insn_ops));
      if (func != NULL)
	MIR_append_insn (context, func, insn);
    }
  }
  if (func != NULL)
    process_error (context, MIR_syntax_error, "absent endfunc");
  if (module != NULL)
    process_error (context, MIR_syntax_error, "absent endmodule");
}

static void scan_init (MIR_context_t context) {
  insn_name_t in, el;
  size_t i;
  
  if ((context->scan_context = malloc (sizeof (struct scan_context))) == NULL)
    (*error_func) (MIR_alloc_error, "Not enough memory for context");
  VARR_CREATE (label_name_t, label_names, 0);
  HTAB_CREATE (label_desc_t, label_desc_tab, 100, label_hash, label_eq);
  HTAB_CREATE (insn_name_t, insn_name_tab, MIR_INSN_BOUND, insn_name_hash, insn_name_eq);
  for (i = 0; i < MIR_INSN_BOUND; i++) {
    in.code = i;
    in.name = MIR_insn_name (context, i);
    HTAB_DO (insn_name_t, insn_name_tab, in, HTAB_INSERT, el);
  }
}

static void scan_finish (MIR_context_t context) {
  VARR_DESTROY (label_name_t, label_names);
  HTAB_DESTROY (label_desc_t, label_desc_tab);
  HTAB_DESTROY (insn_name_t, insn_name_tab);
  free (context->scan_context); context->scan_context = NULL;
}

#endif /* if MIR_SCAN */



#if defined(__x86_64__)
#include "mir-x86_64.c"
#elif defined(__PPC64__)
#include "mir-ppc64.c"
#elif defined(__aarch64__)
#include "mir-aarch64.c"
#else
#error "undefined or unsupported generation target"
#endif



#include "mir-interp.c"
