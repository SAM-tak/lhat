//
// $Id: lcode.c,v 2.112 2016/12/22 13:08:50 roberto Exp $
// Code generator for Lhat
// See Copyright Notice in lhat.h
//

#define lcode_c
#define LHAT_CORE

#include "lprefix.h"


#include <math.h>
#include <stdlib.h>

#include "lhat.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"


// Maximum number of registers in a Lhat function (must fit in 8 bits)
#define MAXREGS		255


#define hasjumps(e)	((e)->t != (e)->f)


//
// If expression is a numeric constant, fills 'v' with its value
// and returns 1. Otherwise, returns 0.
//
static int tonumeral(const expdesc *e, TValue *v) {
  if (hasjumps(e))
    return 0;  // not a numeral
  switch (e->k) {
    case VKINT:
      if (v) setivalue(v, e->u.ival);
      return 1;
    case VKFLT:
      if (v) setfltvalue(v, e->u.nval);
      return 1;
    default: return 0;
  }
}


//
// Create a OP_LOADNIL instruction, but try to optimize: if the previous
// instruction is also OP_LOADNIL and ranges are compatible, adjust
// range of previous instruction instead of emitting a new one. (For
// instance, 'local a; local b' will generate a single opcode.)
//
void lhatK_nil (FuncState *fs, int from, int n) {
  Instruction *previous;
  int l = from + n - 1;  // last register to set nil
  if (fs->pc > fs->lasttarget) {  // no jumps to current position?
    previous = &fs->f->code[fs->pc-1];
    if (GET_OPCODE(*previous) == OP_LOADNIL) {  // previous is LOADNIL?
      int pfrom = GETARG_A(*previous);  // get previous range
      int pl = pfrom + GETARG_B(*previous);
      if ((pfrom <= from && from <= pl + 1) ||
          (from <= pfrom && pfrom <= l + 1)) {  // can connect both?
        if (pfrom < from) from = pfrom;  // from = min(from, pfrom)
        if (pl > l) l = pl;  // l = max(l, pl)
        SETARG_A(*previous, from);
        SETARG_B(*previous, l - from);
        return;
      }
    }  // else go through
  }
  lhatK_codeABC(fs, OP_LOADNIL, from, n - 1, 0);  // else no optimization
}


//
// Gets the destination address of a jump instruction. Used to traverse
// a list of jumps.
//
static int getjump (FuncState *fs, int pc) {
  int offset = GETARG_sBx(fs->f->code[pc]);
  if (offset == NO_JUMP)  // point to itself represents end of list
    return NO_JUMP;  // end of list
  else
    return (pc+1)+offset;  // turn offset into absolute position
}


//
// Fix jump instruction at position 'pc' to jump to 'dest'.
// (Jump addresses are relative in Lhat)
//
static void fixjump (FuncState *fs, int pc, int dest) {
  Instruction *jmp = &fs->f->code[pc];
  int offset = dest - (pc + 1);
  lhat_assert(dest != NO_JUMP);
  if (abs(offset) > MAXARG_sBx)
    lhatX_syntaxerror(fs->ls, "control structure too long");
  SETARG_sBx(*jmp, offset);
}


//
// Concatenate jump-list 'l2' into jump-list 'l1'
//
void lhatK_concat (FuncState *fs, int *l1, int l2) {
  if (l2 == NO_JUMP) return;  // nothing to concatenate?
  else if (*l1 == NO_JUMP)  // no original list?
    *l1 = l2;  // 'l1' points to 'l2'
  else {
    int list = *l1;
    int next;
    while ((next = getjump(fs, list)) != NO_JUMP)  // find last element
      list = next;
    fixjump(fs, list, l2);  // last element links to 'l2'
  }
}


//
// Create a jump instruction and return its position, so its destination
// can be fixed later (with 'fixjump'). If there are jumps to
// this position (kept in 'jpc'), link them all together so that
// 'patchlistaux' will fix all them directly to the final destination.
//
int lhatK_jump (FuncState *fs) {
  int jpc = fs->jpc;  // save list of jumps to here
  int j;
  fs->jpc = NO_JUMP;  // no more jumps to here
  j = lhatK_codeAsBx(fs, OP_JMP, 0, NO_JUMP);
  lhatK_concat(fs, &j, jpc);  // keep them on hold
  return j;
}


//
// Code a 'return' instruction
//
void lhatK_ret (FuncState *fs, int first, int nret) {
  lhatK_codeABC(fs, OP_RETURN, first, nret+1, 0);
}


//
// Code a "conditional jump", that is, a test or comparison opcode
// followed by a jump. Return jump position.
//
static int condjump (FuncState *fs, OpCode op, int A, int B, int C) {
  lhatK_codeABC(fs, op, A, B, C);
  return lhatK_jump(fs);
}


//
// returns current 'pc' and marks it as a jump target (to avoid wrong
// optimizations with consecutive instructions not in the same basic block).
//
int lhatK_getlabel (FuncState *fs) {
  fs->lasttarget = fs->pc;
  return fs->pc;
}


//
// Returns the position of the instruction "controlling" a given
// jump (that is, its condition), or the jump itself if it is
// unconditional.
//
static Instruction *getjumpcontrol (FuncState *fs, int pc) {
  Instruction *pi = &fs->f->code[pc];
  if (pc >= 1 && testTMode(GET_OPCODE(*(pi-1))))
    return pi-1;
  else
    return pi;
}


//
// Patch destination register for a TESTSET instruction.
// If instruction in position 'node' is not a TESTSET, return 0 ("fails").
// Otherwise, if 'reg' is not 'NO_REG', set it as the destination
// register. Otherwise, change instruction to a simple 'TEST' (produces
// no register value)
//
static int patchtestreg (FuncState *fs, int node, int reg) {
  Instruction *i = getjumpcontrol(fs, node);
  if (GET_OPCODE(*i) != OP_TESTSET)
    return 0;  // cannot patch other instructions
  if (reg != NO_REG && reg != GETARG_B(*i))
    SETARG_A(*i, reg);
  else {
     // no register to put value or register already has the value;
     // change instruction to simple test
    *i = CREATE_ABC(OP_TEST, GETARG_B(*i), 0, GETARG_C(*i));
  }
  return 1;
}


//
// Traverse a list of tests ensuring no one produces a value
//
static void removevalues (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list))
      patchtestreg(fs, list, NO_REG);
}


//
// Traverse a list of tests, patching their destination address and
// registers: tests producing values jump to 'vtarget' (and put their
// values in 'reg'), other tests jump to 'dtarget'.
//
static void patchlistaux (FuncState *fs, int list, int vtarget, int reg,
                          int dtarget) {
  while (list != NO_JUMP) {
    int next = getjump(fs, list);
    if (patchtestreg(fs, list, reg))
      fixjump(fs, list, vtarget);
    else
      fixjump(fs, list, dtarget);  // jump to default target
    list = next;
  }
}


//
// Ensure all pending jumps to current position are fixed (jumping
// to current position with no values) and reset list of pending
// jumps
//
static void dischargejpc (FuncState *fs) {
  patchlistaux(fs, fs->jpc, fs->pc, NO_REG, fs->pc);
  fs->jpc = NO_JUMP;
}


//
// Add elements in 'list' to list of pending jumps to "here"
// (current position)
//
void lhatK_patchtohere (FuncState *fs, int list) {
  lhatK_getlabel(fs);  // mark "here" as a jump target
  lhatK_concat(fs, &fs->jpc, list);
}


//
// Path all jumps in 'list' to jump to 'target'.
// (The assert means that we cannot fix a jump to a forward address
// because we only know addresses once code is generated.)
//
void lhatK_patchlist (FuncState *fs, int list, int target) {
  if (target == fs->pc)  // 'target' is current position?
    lhatK_patchtohere(fs, list);  // add list to pending jumps
  else {
    lhat_assert(target < fs->pc);
    patchlistaux(fs, list, target, NO_REG, target);
  }
}


//
// Path all jumps in 'list' to close upvalues up to given 'level'
// (The assertion checks that jumps either were closing nothing
// or were closing higher levels, from inner blocks.)
//
void lhatK_patchclose (FuncState *fs, int list, int level) {
  level++;  // argument is +1 to reserve 0 as non-op
  for (; list != NO_JUMP; list = getjump(fs, list)) {
    lhat_assert(GET_OPCODE(fs->f->code[list]) == OP_JMP &&
                (GETARG_A(fs->f->code[list]) == 0 ||
                 GETARG_A(fs->f->code[list]) >= level));
    SETARG_A(fs->f->code[list], level);
  }
}


//
// Emit instruction 'i', checking for array sizes and saving also its
// line information. Return 'i' position.
//
static int lhatK_code (FuncState *fs, Instruction i) {
  Proto *f = fs->f;
  dischargejpc(fs);  // 'pc' will change
  // put new instruction in code array
  lhatM_growvector(fs->ls->L, f->code, fs->pc, f->sizecode, Instruction,
                  MAX_INT, "opcodes");
  f->code[fs->pc] = i;
  // save corresponding line information
  lhatM_growvector(fs->ls->L, f->lineinfo, fs->pc, f->sizelineinfo, int,
                  MAX_INT, "opcodes");
  f->lineinfo[fs->pc] = fs->ls->lastline;
  return fs->pc++;
}


//
// Format and emit an 'iABC' instruction. (Assertions check consistency
// of parameters versus opcode.)
//
int lhatK_codeABC (FuncState *fs, OpCode o, int a, int b, int c) {
  lhat_assert(getOpMode(o) == iABC);
  lhat_assert(getBMode(o) != OpArgN || b == 0);
  lhat_assert(getCMode(o) != OpArgN || c == 0);
  lhat_assert(a <= MAXARG_A && b <= MAXARG_B && c <= MAXARG_C);
  return lhatK_code(fs, CREATE_ABC(o, a, b, c));
}


//
// Format and emit an 'iABx' instruction.
//
int lhatK_codeABx (FuncState *fs, OpCode o, int a, unsigned int bc) {
  lhat_assert(getOpMode(o) == iABx || getOpMode(o) == iAsBx);
  lhat_assert(getCMode(o) == OpArgN);
  lhat_assert(a <= MAXARG_A && bc <= MAXARG_Bx);
  return lhatK_code(fs, CREATE_ABx(o, a, bc));
}


//
// Emit an "extra argument" instruction (format 'iAx')
//
static int codeextraarg (FuncState *fs, int a) {
  lhat_assert(a <= MAXARG_Ax);
  return lhatK_code(fs, CREATE_Ax(OP_EXTRAARG, a));
}


//
// Emit a "load constant" instruction, using either 'OP_LOADK'
// (if constant index 'k' fits in 18 bits) or an 'OP_LOADKX'
// instruction with "extra argument".
//
int lhatK_codek (FuncState *fs, int reg, int k) {
  if (k <= MAXARG_Bx)
    return lhatK_codeABx(fs, OP_LOADK, reg, k);
  else {
    int p = lhatK_codeABx(fs, OP_LOADKX, reg, 0);
    codeextraarg(fs, k);
    return p;
  }
}


//
// Check register-stack level, keeping track of its maximum size
// in field 'maxstacksize'
//
void lhatK_checkstack (FuncState *fs, int n) {
  int newstack = fs->freereg + n;
  if (newstack > fs->f->maxstacksize) {
    if (newstack >= MAXREGS)
      lhatX_syntaxerror(fs->ls,
        "function or expression needs too many registers");
    fs->f->maxstacksize = cast_byte(newstack);
  }
}


//
// Reserve 'n' registers in register stack
//
void lhatK_reserveregs (FuncState *fs, int n) {
  lhatK_checkstack(fs, n);
  fs->freereg += n;
}


//
// Free register 'reg', if it is neither a constant index nor
// a local variable.
//
static void freereg (FuncState *fs, int reg) {
  if (!ISK(reg) && reg >= fs->nactvar) {
    fs->freereg--;
    lhat_assert(reg == fs->freereg);
  }
}


//
// Free register used by expression 'e' (if any)
//
static void freeexp (FuncState *fs, expdesc *e) {
  if (e->k == VNONRELOC)
    freereg(fs, e->u.info);
}


//
// Free registers used by expressions 'e1' and 'e2' (if any) in proper
// order.
//
static void freeexps (FuncState *fs, expdesc *e1, expdesc *e2) {
  int r1 = (e1->k == VNONRELOC) ? e1->u.info : -1;
  int r2 = (e2->k == VNONRELOC) ? e2->u.info : -1;
  if (r1 > r2) {
    freereg(fs, r1);
    freereg(fs, r2);
  }
  else {
    freereg(fs, r2);
    freereg(fs, r1);
  }
}


//
// Add constant 'v' to prototype's list of constants (field 'k').
// Use scanner's table to cache position of constants in constant list
// and try to reuse constants. Because some values should not be used
// as keys (nil cannot be a key, integer keys can collapse with float
// keys), the caller must provide a useful 'key' for indexing the cache.
//
static int addk (FuncState *fs, TValue *key, TValue *v) {
  lhat_State *L = fs->ls->L;
  Proto *f = fs->f;
  TValue *idx = lhatH_set(L, fs->ls->h, key);  // index scanner table
  int k, oldsize;
  if (ttisinteger(idx)) {  // is there an index there?
    k = cast_int(ivalue(idx));
    // correct value? (warning: must distinguish floats from integers!)
    if (k < fs->nk && ttype(&f->k[k]) == ttype(v) &&
                      lhatV_rawequalobj(&f->k[k], v))
      return k;  // reuse index
  }
  // constant not found; create a new entry
  oldsize = f->sizek;
  k = fs->nk;
  // numerical value does not need GC barrier;
  // table has no metatable, so it does not need to invalidate cache
  setivalue(idx, k);
  lhatM_growvector(L, f->k, k, f->sizek, TValue, MAXARG_Ax, "constants");
  while (oldsize < f->sizek) setnilvalue(&f->k[oldsize++]);
  setobj(L, &f->k[k], v);
  fs->nk++;
  lhatC_barrier(L, f, v);
  return k;
}


//
// Add a string to list of constants and return its index.
//
int lhatK_stringK (FuncState *fs, TString *s) {
  TValue o;
  setsvalue(fs->ls->L, &o, s);
  return addk(fs, &o, &o);  // use string itself as key
}


//
// Add an integer to list of constants and return its index.
// Integers use userdata as keys to avoid collision with floats with
// same value; conversion to 'void*' is used only for hashing, so there
// are no "precision" problems.
//
int lhatK_intK (FuncState *fs, lhat_Integer n) {
  TValue k, o;
  setpvalue(&k, cast(void*, cast(size_t, n)));
  setivalue(&o, n);
  return addk(fs, &k, &o);
}

//
// Add a float to list of constants and return its index.
//
static int lhatK_numberK (FuncState *fs, lhat_Number r) {
  TValue o;
  setfltvalue(&o, r);
  return addk(fs, &o, &o);  // use number itself as key
}


//
// Add a boolean to list of constants and return its index.
//
static int boolK (FuncState *fs, int b) {
  TValue o;
  setbvalue(&o, b);
  return addk(fs, &o, &o);  // use boolean itself as key
}


//
// Add nil to list of constants and return its index.
//
static int nilK (FuncState *fs) {
  TValue k, v;
  setnilvalue(&v);
  // cannot use nil as key; instead use table itself to represent nil
  sethvalue(fs->ls->L, &k, fs->ls->h);
  return addk(fs, &k, &v);
}


//
// Fix an expression to return the number of results 'nresults'.
// Either 'e' is a multi-ret expression (function call or vararg)
// or 'nresults' is LHAT_MULTRET (as any expression can satisfy that).
//
void lhatK_setreturns (FuncState *fs, expdesc *e, int nresults) {
  if (e->k == VCALL) {  // expression is an open function call?
    SETARG_C(getinstruction(fs, e), nresults + 1);
  }
  else if (e->k == VVARARG) {
    Instruction *pc = &getinstruction(fs, e);
    SETARG_B(*pc, nresults + 1);
    SETARG_A(*pc, fs->freereg);
    lhatK_reserveregs(fs, 1);
  }
  else lhat_assert(nresults == LHAT_MULTRET);
}


//
// Fix an expression to return one result.
// If expression is not a multi-ret expression (function call or
// vararg), it already returns one result, so nothing needs to be done.
// Function calls become VNONRELOC expressions (as its result comes
// fixed in the base register of the call), while vararg expressions
// become VRELOCABLE (as OP_VARARG puts its results where it wants).
// (Calls are created returning one result, so that does not need
// to be fixed.)
//
void lhatK_setoneret (FuncState *fs, expdesc *e) {
  if (e->k == VCALL) {  // expression is an open function call?
    // already returns 1 value
    lhat_assert(GETARG_C(getinstruction(fs, e)) == 2);
    e->k = VNONRELOC;  // result has fixed position
    e->u.info = GETARG_A(getinstruction(fs, e));
  }
  else if (e->k == VVARARG) {
    SETARG_B(getinstruction(fs, e), 2);
    e->k = VRELOCABLE;  // can relocate its simple result
  }
}


//
// Ensure that expression 'e' is not a variable.
//
void lhatK_dischargevars (FuncState *fs, expdesc *e) {
  switch (e->k) {
    case VLOCAL: {  // already in a register
      e->k = VNONRELOC;  // becomes a non-relocatable value
      break;
    }
    case VUPVAL: {  // move value to some (pending) register
      e->u.info = lhatK_codeABC(fs, OP_GETUPVAL, 0, e->u.info, 0);
      e->k = VRELOCABLE;
      break;
    }
    case VINDEXED: {
      OpCode op;
      freereg(fs, e->u.ind.idx);
      if (e->u.ind.vt == VLOCAL) {  // is 't' in a register?
        freereg(fs, e->u.ind.t);
        op = OP_GETTABLE;
      }
      else {
        lhat_assert(e->u.ind.vt == VUPVAL);
        op = OP_GETTABUP;  // 't' is in an upvalues
      }
      e->u.info = lhatK_codeABC(fs, op, 0, e->u.ind.t, e->u.ind.idx);
      e->k = VRELOCABLE;
      break;
    }
    case VVARARG: case VCALL: {
      lhatK_setoneret(fs, e);
      break;
    }
    default: break;  // there is one value available (somewhere)
  }
}


//
// Ensures expression value is in register 'reg' (and therefore
// 'e' will become a non-relocatable expression).
//
static void discharge2reg (FuncState *fs, expdesc *e, int reg) {
  lhatK_dischargevars(fs, e);
  switch (e->k) {
    case VNIL: {
      lhatK_nil(fs, reg, 1);
      break;
    }
    case VFALSE: case VTRUE: {
      lhatK_codeABC(fs, OP_LOADBOOL, reg, e->k == VTRUE, 0);
      break;
    }
    case VK: {
      lhatK_codek(fs, reg, e->u.info);
      break;
    }
    case VKFLT: {
      lhatK_codek(fs, reg, lhatK_numberK(fs, e->u.nval));
      break;
    }
    case VKINT: {
      lhatK_codek(fs, reg, lhatK_intK(fs, e->u.ival));
      break;
    }
    case VRELOCABLE: {
      Instruction *pc = &getinstruction(fs, e);
      SETARG_A(*pc, reg);  // instruction will put result in 'reg'
      break;
    }
    case VNONRELOC: {
      if (reg != e->u.info)
        lhatK_codeABC(fs, OP_MOVE, reg, e->u.info, 0);
      break;
    }
    default: {
      lhat_assert(e->k == VJMP);
      return;  // nothing to do...
    }
  }
  e->u.info = reg;
  e->k = VNONRELOC;
}


//
// Ensures expression value is in any register.
//
static void discharge2anyreg (FuncState *fs, expdesc *e) {
  if (e->k != VNONRELOC) {  // no fixed register yet?
    lhatK_reserveregs(fs, 1);  // get a register
    discharge2reg(fs, e, fs->freereg-1);  // put value there
  }
}


static int code_loadbool (FuncState *fs, int A, int b, int jump) {
  lhatK_getlabel(fs);  // those instructions may be jump targets
  return lhatK_codeABC(fs, OP_LOADBOOL, A, b, jump);
}


//
// check whether list has any jump that do not produce a value
// or produce an inverted value
//
static int need_value (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list)) {
    Instruction i = *getjumpcontrol(fs, list);
    if (GET_OPCODE(i) != OP_TESTSET) return 1;
  }
  return 0;  // not found
}


//
// Ensures final expression result (including results from its jump
// lists) is in register 'reg'.
// If expression has jumps, need to patch these jumps either to
// its final position or to "load" instructions (for those tests
// that do not produce values).
//
static void exp2reg (FuncState *fs, expdesc *e, int reg) {
  discharge2reg(fs, e, reg);
  if (e->k == VJMP)  // expression itself is a test?
    lhatK_concat(fs, &e->t, e->u.info);  // put this jump in 't' list
  if (hasjumps(e)) {
    int final;  // position after whole expression
    int p_f = NO_JUMP;  // position of an eventual LOAD false
    int p_t = NO_JUMP;  // position of an eventual LOAD true
    if (need_value(fs, e->t) || need_value(fs, e->f)) {
      int fj = (e->k == VJMP) ? NO_JUMP : lhatK_jump(fs);
      p_f = code_loadbool(fs, reg, 0, 1);
      p_t = code_loadbool(fs, reg, 1, 0);
      lhatK_patchtohere(fs, fj);
    }
    final = lhatK_getlabel(fs);
    patchlistaux(fs, e->f, final, reg, p_f);
    patchlistaux(fs, e->t, final, reg, p_t);
  }
  e->f = e->t = NO_JUMP;
  e->u.info = reg;
  e->k = VNONRELOC;
}


//
// Ensures final expression result (including results from its jump
// lists) is in next available register.
//
void lhatK_exp2nextreg (FuncState *fs, expdesc *e) {
  lhatK_dischargevars(fs, e);
  freeexp(fs, e);
  lhatK_reserveregs(fs, 1);
  exp2reg(fs, e, fs->freereg - 1);
}


//
// Ensures final expression result (including results from its jump
// lists) is in some (any) register and return that register.
//
int lhatK_exp2anyreg (FuncState *fs, expdesc *e) {
  lhatK_dischargevars(fs, e);
  if (e->k == VNONRELOC) {  // expression already has a register?
    if (!hasjumps(e))  // no jumps?
      return e->u.info;  // result is already in a register
    if (e->u.info >= fs->nactvar) {  // reg. is not a local?
      exp2reg(fs, e, e->u.info);  // put final result in it
      return e->u.info;
    }
  }
  lhatK_exp2nextreg(fs, e);  // otherwise, use next available register
  return e->u.info;
}


//
// Ensures final expression result is either in a register or in an
// upvalues.
//
void lhatK_exp2anyregup (FuncState *fs, expdesc *e) {
  if (e->k != VUPVAL || hasjumps(e))
    lhatK_exp2anyreg(fs, e);
}


//
// Ensures final expression result is either in a register or it is
// a constant.
//
void lhatK_exp2val (FuncState *fs, expdesc *e) {
  if (hasjumps(e))
    lhatK_exp2anyreg(fs, e);
  else
    lhatK_dischargevars(fs, e);
}


//
// Ensures final expression result is in a valid R/K index
// (that is, it is either in a register or in 'k' with an index
// in the range of R/K indices).
// Returns R/K index.
//
int lhatK_exp2RK (FuncState *fs, expdesc *e) {
  lhatK_exp2val(fs, e);
  switch (e->k) {  // move constants to 'k'
    case VTRUE: e->u.info = boolK(fs, 1); goto vk;
    case VFALSE: e->u.info = boolK(fs, 0); goto vk;
    case VNIL: e->u.info = nilK(fs); goto vk;
    case VKINT: e->u.info = lhatK_intK(fs, e->u.ival); goto vk;
    case VKFLT: e->u.info = lhatK_numberK(fs, e->u.nval); goto vk;
    case VK:
     vk:
      e->k = VK;
      if (e->u.info <= MAXINDEXRK)  // constant fits in 'argC'?
        return RKASK(e->u.info);
      else break;
    default: break;
  }
  // not a constant in the right range: put it in a register
  return lhatK_exp2anyreg(fs, e);
}


//
// Generate code to store result of expression 'ex' into variable 'var'.
//
void lhatK_storevar (FuncState *fs, expdesc *var, expdesc *ex) {
  switch (var->k) {
    case VLOCAL: {
      freeexp(fs, ex);
      exp2reg(fs, ex, var->u.info);  // compute 'ex' into proper place
      return;
    }
    case VUPVAL: {
      int e = lhatK_exp2anyreg(fs, ex);
      lhatK_codeABC(fs, OP_SETUPVAL, e, var->u.info, 0);
      break;
    }
    case VINDEXED: {
      OpCode op = (var->u.ind.vt == VLOCAL) ? OP_SETTABLE : OP_SETTABUP;
      int e = lhatK_exp2RK(fs, ex);
      lhatK_codeABC(fs, op, var->u.ind.t, var->u.ind.idx, e);
      break;
    }
    default: lhat_assert(0);  // invalid var kind to store
  }
  freeexp(fs, ex);
}


//
// Emit SELF instruction (convert expression 'e' into 'e:key(e,').
//
void lhatK_self (FuncState *fs, expdesc *e, expdesc *key) {
  int ereg;
  lhatK_exp2anyreg(fs, e);
  ereg = e->u.info;  // register where 'e' was placed
  freeexp(fs, e);
  e->u.info = fs->freereg;  // base register for op_self
  e->k = VNONRELOC;  // self expression has a fixed register
  lhatK_reserveregs(fs, 2);  // function and 'self' produced by op_self
  lhatK_codeABC(fs, OP_SELF, e->u.info, ereg, lhatK_exp2RK(fs, key));
  freeexp(fs, key);
}


//
// Negate condition 'e' (where 'e' is a comparison).
//
static void negatecondition (FuncState *fs, expdesc *e) {
  Instruction *pc = getjumpcontrol(fs, e->u.info);
  lhat_assert(testTMode(GET_OPCODE(*pc)) && GET_OPCODE(*pc) != OP_TESTSET &&
                                           GET_OPCODE(*pc) != OP_TEST);
  SETARG_A(*pc, !(GETARG_A(*pc)));
}


//
// Emit instruction to jump if 'e' is 'cond' (that is, if 'cond'
// is true, code will jump if 'e' is true.) Return jump position.
// Optimize when 'e' is 'not' something, inverting the condition
// and removing the 'not'.
//
static int jumponcond (FuncState *fs, expdesc *e, int cond) {
  if (e->k == VRELOCABLE) {
    Instruction ie = getinstruction(fs, e);
    if (GET_OPCODE(ie) == OP_NOT) {
      fs->pc--;  // remove previous OP_NOT
      return condjump(fs, OP_TEST, GETARG_B(ie), 0, !cond);
    }
    // else go through
  }
  discharge2anyreg(fs, e);
  freeexp(fs, e);
  return condjump(fs, OP_TESTSET, NO_REG, e->u.info, cond);
}


//
// Emit code to go through if 'e' is true, jump otherwise.
//
void lhatK_goiftrue (FuncState *fs, expdesc *e) {
  int pc;  // pc of new jump
  lhatK_dischargevars(fs, e);
  switch (e->k) {
    case VJMP: {  // condition?
      negatecondition(fs, e);  // jump when it is false
      pc = e->u.info;  // save jump position
      break;
    }
    case VK: case VKFLT: case VKINT: case VTRUE: {
      pc = NO_JUMP;  // always true; do nothing
      break;
    }
    default: {
      pc = jumponcond(fs, e, 0);  // jump when false
      break;
    }
  }
  lhatK_concat(fs, &e->f, pc);  // insert new jump in false list
  lhatK_patchtohere(fs, e->t);  // true list jumps to here (to go through)
  e->t = NO_JUMP;
}


//
// Emit code to go through if 'e' is false, jump otherwise.
//
void lhatK_goiffalse (FuncState *fs, expdesc *e) {
  int pc;  // pc of new jump
  lhatK_dischargevars(fs, e);
  switch (e->k) {
    case VJMP: {
      pc = e->u.info;  // already jump if true
      break;
    }
    case VNIL: case VFALSE: {
      pc = NO_JUMP;  // always false; do nothing
      break;
    }
    default: {
      pc = jumponcond(fs, e, 1);  // jump if true
      break;
    }
  }
  lhatK_concat(fs, &e->t, pc);  // insert new jump in 't' list
  lhatK_patchtohere(fs, e->f);  // false list jumps to here (to go through)
  e->f = NO_JUMP;
}


//
// Code 'not e', doing constant folding.
//
static void codenot (FuncState *fs, expdesc *e) {
  lhatK_dischargevars(fs, e);
  switch (e->k) {
    case VNIL: case VFALSE: {
      e->k = VTRUE;  // true == not nil == not false
      break;
    }
    case VK: case VKFLT: case VKINT: case VTRUE: {
      e->k = VFALSE;  // false == not "x" == not 0.5 == not 1 == not true
      break;
    }
    case VJMP: {
      negatecondition(fs, e);
      break;
    }
    case VRELOCABLE:
    case VNONRELOC: {
      discharge2anyreg(fs, e);
      freeexp(fs, e);
      e->u.info = lhatK_codeABC(fs, OP_NOT, 0, e->u.info, 0);
      e->k = VRELOCABLE;
      break;
    }
    default: lhat_assert(0);  // cannot happen
  }
  // interchange true and false lists
  { int temp = e->f; e->f = e->t; e->t = temp; }
  removevalues(fs, e->f);  // values are useless when negated
  removevalues(fs, e->t);
}


//
// Create expression 't[k]'. 't' must have its final result already in a
// register or upvalues.
//
void lhatK_indexed (FuncState *fs, expdesc *t, expdesc *k) {
  lhat_assert(!hasjumps(t) && (vkisinreg(t->k) || t->k == VUPVAL));
  t->u.ind.t = t->u.info;  // register or upvalues index
  t->u.ind.idx = lhatK_exp2RK(fs, k);  // R/K index for key
  t->u.ind.vt = (t->k == VUPVAL) ? VUPVAL : VLOCAL;
  t->k = VINDEXED;
}


//
// Return false if folding can raise an error.
// Bitwise operations need operands convertible to integers; division
// operations cannot have 0 as divisor.
//
static int validop (int op, TValue *v1, TValue *v2) {
  switch (op) {
    case LHAT_OPBAND: case LHAT_OPBOR: case LHAT_OPBXOR:
    case LHAT_OPSHL: case LHAT_OPSHR: case LHAT_OPBNOT: {  // conversion errors
      lhat_Integer i;
      return (tointeger(v1, &i) && tointeger(v2, &i));
    }
    case LHAT_OPDIV: case LHAT_OPIDIV: case LHAT_OPMOD:  // division by 0
      return (nvalue(v2) != 0);
    default: return 1;  // everything else is valid
  }
}


//
// Try to "constant-fold" an operation; return 1 iff successful.
// (In this case, 'e1' has the final result.)
//
static int constfolding (FuncState *fs, int op, expdesc *e1,
                                                const expdesc *e2) {
  TValue v1, v2, res;
  if (!tonumeral(e1, &v1) || !tonumeral(e2, &v2) || !validop(op, &v1, &v2))
    return 0;  // non-numeric operands or not safe to fold
  lhatO_arith(fs->ls->L, op, &v1, &v2, &res);  // does operation
  if (ttisinteger(&res)) {
    e1->k = VKINT;
    e1->u.ival = ivalue(&res);
  }
  else {  // folds neither NaN nor 0.0 (to avoid problems with -0.0)
    lhat_Number n = fltvalue(&res);
    if (lhati_numisnan(n) || n == 0)
      return 0;
    e1->k = VKFLT;
    e1->u.nval = n;
  }
  return 1;
}


//
// Emit code for unary expressions that "produce values"
// (everything but 'not').
// Expression to produce final result will be encoded in 'e'.
//
static void codeunexpval (FuncState *fs, OpCode op, expdesc *e, int line) {
  int r = lhatK_exp2anyreg(fs, e);  // opcodes operate only on registers
  freeexp(fs, e);
  e->u.info = lhatK_codeABC(fs, op, 0, r, 0);  // generate opcode
  e->k = VRELOCABLE;  // all those operations are relocatable
  lhatK_fixline(fs, line);
}


//
// Emit code for binary expressions that "produce values"
// (everything but logical operators 'and'/'or' and comparison
// operators).
// Expression to produce final result will be encoded in 'e1'.
// Because 'lhatK_exp2RK' can free registers, its calls must be
// in "stack order" (that is, first on 'e2', which may have more
// recent registers to be released).
//
static void codebinexpval (FuncState *fs, OpCode op,
                           expdesc *e1, expdesc *e2, int line) {
  int rk2 = lhatK_exp2RK(fs, e2);  // both operands are "RK"
  int rk1 = lhatK_exp2RK(fs, e1);
  freeexps(fs, e1, e2);
  e1->u.info = lhatK_codeABC(fs, op, 0, rk1, rk2);  // generate opcode
  e1->k = VRELOCABLE;  // all those operations are relocatable
  lhatK_fixline(fs, line);
}


//
// Emit code for comparisons.
// 'e1' was already put in R/K form by 'lhatK_infix'.
//
static void codecomp (FuncState *fs, BinOpr opr, expdesc *e1, expdesc *e2) {
  int rk1 = (e1->k == VK) ? RKASK(e1->u.info)
                          : check_exp(e1->k == VNONRELOC, e1->u.info);
  int rk2 = lhatK_exp2RK(fs, e2);
  freeexps(fs, e1, e2);
  switch (opr) {
    case OPR_NE: {  // '(a ~= b)' ==> 'not (a == b)'
      e1->u.info = condjump(fs, OP_EQ, 0, rk1, rk2);
      break;
    }
    case OPR_GT: case OPR_GE: {
      // '(a > b)' ==> '(b < a)';  '(a >= b)' ==> '(b <= a)'
      OpCode op = cast(OpCode, (opr - OPR_NE) + OP_EQ);
      e1->u.info = condjump(fs, op, 1, rk2, rk1);  // invert operands
      break;
    }
    default: {  // '==', '<', '<=' use their own opcodes
      OpCode op = cast(OpCode, (opr - OPR_EQ) + OP_EQ);
      e1->u.info = condjump(fs, op, 1, rk1, rk2);
      break;
    }
  }
  e1->k = VJMP;
}


//
// Aplly prefix operation 'op' to expression 'e'.
//
void lhatK_prefix (FuncState *fs, UnOpr op, expdesc *e, int line) {
  static const expdesc ef = {VKINT, {0}, NO_JUMP, NO_JUMP};
  switch (op) {
    case OPR_MINUS: case OPR_BNOT:  // use 'ef' as fake 2nd operand
      if (constfolding(fs, op + LHAT_OPUNM, e, &ef))
        break;
      // FALLTHROUGH
    case OPR_LEN:
      codeunexpval(fs, cast(OpCode, op + OP_UNM), e, line);
      break;
    case OPR_NOT: codenot(fs, e); break;
    default: lhat_assert(0);
  }
}


//
// Process 1st operand 'v' of binary operation 'op' before reading
// 2nd operand.
//
void lhatK_infix (FuncState *fs, BinOpr op, expdesc *v) {
  switch (op) {
    case OPR_AND: {
      lhatK_goiftrue(fs, v);  // go ahead only if 'v' is true
      break;
    }
    case OPR_OR: {
      lhatK_goiffalse(fs, v);  // go ahead only if 'v' is false
      break;
    }
    case OPR_CONCAT: {
      lhatK_exp2nextreg(fs, v);  // operand must be on the 'stack'
      break;
    }
    case OPR_ADD: case OPR_SUB:
    case OPR_MUL: case OPR_DIV: case OPR_IDIV:
    case OPR_MOD: case OPR_POW:
    case OPR_BAND: case OPR_BOR: case OPR_BXOR:
    case OPR_SHL: case OPR_SHR: {
      if (!tonumeral(v, NULL))
        lhatK_exp2RK(fs, v);
      // else keep numeral, which may be folded with 2nd operand
      break;
    }
    default: {
      lhatK_exp2RK(fs, v);
      break;
    }
  }
}


//
// Finalize code for binary operation, after reading 2nd operand.
// For '(a .. b .. c)' (which is '(a .. (b .. c))', because
// concatenation is right associative), merge second CONCAT into first
// one.
//
void lhatK_posfix (FuncState *fs, BinOpr op,
                  expdesc *e1, expdesc *e2, int line) {
  switch (op) {
    case OPR_AND: {
      lhat_assert(e1->t == NO_JUMP);  // list closed by 'luK_infix'
      lhatK_dischargevars(fs, e2);
      lhatK_concat(fs, &e2->f, e1->f);
      *e1 = *e2;
      break;
    }
    case OPR_OR: {
      lhat_assert(e1->f == NO_JUMP);  // list closed by 'luK_infix'
      lhatK_dischargevars(fs, e2);
      lhatK_concat(fs, &e2->t, e1->t);
      *e1 = *e2;
      break;
    }
    case OPR_CONCAT: {
      lhatK_exp2val(fs, e2);
      if (e2->k == VRELOCABLE &&
          GET_OPCODE(getinstruction(fs, e2)) == OP_CONCAT) {
        lhat_assert(e1->u.info == GETARG_B(getinstruction(fs, e2))-1);
        freeexp(fs, e1);
        SETARG_B(getinstruction(fs, e2), e1->u.info);
        e1->k = VRELOCABLE; e1->u.info = e2->u.info;
      }
      else {
        lhatK_exp2nextreg(fs, e2);  // operand must be on the 'stack'
        codebinexpval(fs, OP_CONCAT, e1, e2, line);
      }
      break;
    }
    case OPR_ADD: case OPR_SUB: case OPR_MUL: case OPR_DIV:
    case OPR_IDIV: case OPR_MOD: case OPR_POW:
    case OPR_BAND: case OPR_BOR: case OPR_BXOR:
    case OPR_SHL: case OPR_SHR: {
      if (!constfolding(fs, op + LHAT_OPADD, e1, e2))
        codebinexpval(fs, cast(OpCode, op + OP_ADD), e1, e2, line);
      break;
    }
    case OPR_EQ: case OPR_LT: case OPR_LE:
    case OPR_NE: case OPR_GT: case OPR_GE: {
      codecomp(fs, op, e1, e2);
      break;
    }
    default: lhat_assert(0);
  }
}


//
// Change line information associated with current position.
//
void lhatK_fixline (FuncState *fs, int line) {
  fs->f->lineinfo[fs->pc - 1] = line;
}


//
// Emit a SETLIST instruction.
// 'base' is register that keeps table;
// 'nelems' is #table plus those to be stored now;
// 'tostore' is number of values (in registers 'base + 1',...) to add to
// table (or LHAT_MULTRET to add up to stack top).
//
void lhatK_setlist (FuncState *fs, int base, int nelems, int tostore) {
  int c =  (nelems - 1)/LFIELDS_PER_FLUSH + 1;
  int b = (tostore == LHAT_MULTRET) ? 0 : tostore;
  lhat_assert(tostore != 0 && tostore <= LFIELDS_PER_FLUSH);
  if (c <= MAXARG_C)
    lhatK_codeABC(fs, OP_SETLIST, base, b, c);
  else if (c <= MAXARG_Ax) {
    lhatK_codeABC(fs, OP_SETLIST, base, b, 0);
    codeextraarg(fs, c);
  }
  else
    lhatX_syntaxerror(fs->ls, "constructor too long");
  fs->freereg = base + 1;  // free registers with list values
}

