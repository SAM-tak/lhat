#ifndef lcode_h
#define lcode_h
//
// Code generator for L^
// See Copyright Notice in lhat.h
//

#include "llex.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"


//
// Marks the end of a patch list. It is an invalid value both as an absolute
// address, and as a list link (would link an element to itself).
//
enum {
	NO_JUMP = -1
};


//
// grep "ORDER OPR" if you change these enums  (ORDER OP)
//
typedef enum BinOpr {
	OPR_ADD, OPR_SUB, OPR_MUL, OPR_MOD, OPR_POW,
	OPR_DIV,
	OPR_IDIV,
	OPR_BAND, OPR_BOR, OPR_BXOR,
	OPR_SHL, OPR_SHR,
	OPR_CONCAT,
	OPR_EQ, OPR_LT, OPR_LE,
	OPR_NE, OPR_GT, OPR_GE,
	OPR_AND, OPR_OR,
	OPR_NOBINOPR
} BinOpr;


typedef enum UnOpr { OPR_MINUS, OPR_BNOT, OPR_NOT, OPR_LEN, OPR_NOUNOPR } UnOpr;

// get (pointer to) instruction of given 'ExpDesc'
#define getinstruction(fs,e)	((fs)->f->code[(e)->u.info])

LHATI_FUNC int lhatK_codeABx(FuncState *fs, OpCode o, int A, unsigned int Bx);
LHATI_FUNC int lhatK_codeABC(FuncState *fs, OpCode o, int A, int B, int C);
LHATI_FUNC int lhatK_codek(FuncState *fs, int reg, int k);
LHATI_FUNC void lhatK_fixline(FuncState *fs, int line);
LHATI_FUNC void lhatK_nil(FuncState *fs, int from, int n);
LHATI_FUNC void lhatK_reserveregs(FuncState *fs, int n);
LHATI_FUNC void lhatK_checkstack(FuncState *fs, int n);
LHATI_FUNC int lhatK_stringK(FuncState *fs, TString *s);
LHATI_FUNC int lhatK_intK(FuncState *fs, lhat_Integer n);
LHATI_FUNC void lhatK_dischargevars(FuncState *fs, ExpDesc *e);
LHATI_FUNC int lhatK_exp2anyreg(FuncState *fs, ExpDesc *e);
LHATI_FUNC void lhatK_exp2anyregup(FuncState *fs, ExpDesc *e);
LHATI_FUNC void lhatK_exp2nextreg(FuncState *fs, ExpDesc *e);
LHATI_FUNC void lhatK_exp2val(FuncState *fs, ExpDesc *e);
LHATI_FUNC int lhatK_exp2RK(FuncState *fs, ExpDesc *e);
LHATI_FUNC void lhatK_self(FuncState *fs, ExpDesc *e, ExpDesc *key);
LHATI_FUNC void lhatK_indexed(FuncState *fs, ExpDesc *t, ExpDesc *k);
LHATI_FUNC void lhatK_goiftrue(FuncState *fs, ExpDesc *e);
LHATI_FUNC void lhatK_goiffalse(FuncState *fs, ExpDesc *e);
LHATI_FUNC void lhatK_storevar(FuncState *fs, ExpDesc *var, ExpDesc *e);
LHATI_FUNC void lhatK_setreturns(FuncState *fs, ExpDesc *e, int nresults);
LHATI_FUNC void lhatK_setoneret(FuncState *fs, ExpDesc *e);
LHATI_FUNC int lhatK_jump(FuncState *fs);
LHATI_FUNC void lhatK_ret(FuncState *fs, int first, int nret);
LHATI_FUNC void lhatK_patchlist(FuncState *fs, int list, int target);
LHATI_FUNC void lhatK_patchtohere(FuncState *fs, int list);
LHATI_FUNC void lhatK_patchclose(FuncState *fs, int list, int level);
LHATI_FUNC void lhatK_concat(FuncState *fs, int *l1, int l2);
LHATI_FUNC int lhatK_getlabel(FuncState *fs);
LHATI_FUNC void lhatK_prefix(FuncState *fs, UnOpr op, ExpDesc *v, int line);
LHATI_FUNC void lhatK_infix(FuncState *fs, BinOpr op, ExpDesc *v);
LHATI_FUNC void lhatK_posfix(FuncState *fs, BinOpr op, ExpDesc *v1, ExpDesc *v2, int line);
LHATI_FUNC void lhatK_setlist(FuncState *fs, int base, int nelems, int tostore);

inline int lhatK_codeAsBx(FuncState *fs, OpCode o, int A, unsigned int sBx)
{
	return lhatK_codeABx(fs, o, A, sBx + MAXARG_sBx);
}

inline void lhatK_setmultret(FuncState *fs, ExpDesc *e)
{
	lhatK_setreturns(fs, e, LHAT_MULTRET);
}

inline void lhatK_jumpto(FuncState *fs, int t)
{
	lhatK_patchlist(fs, lhatK_jump(fs), t);
}

#endif