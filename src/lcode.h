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
#define NO_JUMP (-1)


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


// get (pointer to) instruction of given 'expdesc'
#define getinstruction(fs,e)	((fs)->f->code[(e)->u.info])

#define lhatK_codeAsBx(fs,o,A,sBx)	lhatK_codeABx(fs,o,A,(sBx)+MAXARG_sBx)

#define lhatK_setmultret(fs,e)	lhatK_setreturns(fs, e, LHAT_MULTRET)

#define lhatK_jumpto(fs,t)	lhatK_patchlist(fs, lhatK_jump(fs), t)

LHATI_FUNC int lhatK_codeABx(FuncState *fs, OpCode o, int A, unsigned int Bx);
LHATI_FUNC int lhatK_codeABC(FuncState *fs, OpCode o, int A, int B, int C);
LHATI_FUNC int lhatK_codek(FuncState *fs, int reg, int k);
LHATI_FUNC void lhatK_fixline(FuncState *fs, int line);
LHATI_FUNC void lhatK_nil(FuncState *fs, int from, int n);
LHATI_FUNC void lhatK_reserveregs(FuncState *fs, int n);
LHATI_FUNC void lhatK_checkstack(FuncState *fs, int n);
LHATI_FUNC int lhatK_stringK(FuncState *fs, TString *s);
LHATI_FUNC int lhatK_intK(FuncState *fs, lhat_Integer n);
LHATI_FUNC void lhatK_dischargevars(FuncState *fs, expdesc *e);
LHATI_FUNC int lhatK_exp2anyreg(FuncState *fs, expdesc *e);
LHATI_FUNC void lhatK_exp2anyregup(FuncState *fs, expdesc *e);
LHATI_FUNC void lhatK_exp2nextreg(FuncState *fs, expdesc *e);
LHATI_FUNC void lhatK_exp2val(FuncState *fs, expdesc *e);
LHATI_FUNC int lhatK_exp2RK(FuncState *fs, expdesc *e);
LHATI_FUNC void lhatK_self(FuncState *fs, expdesc *e, expdesc *key);
LHATI_FUNC void lhatK_indexed(FuncState *fs, expdesc *t, expdesc *k);
LHATI_FUNC void lhatK_goiftrue(FuncState *fs, expdesc *e);
LHATI_FUNC void lhatK_goiffalse(FuncState *fs, expdesc *e);
LHATI_FUNC void lhatK_storevar(FuncState *fs, expdesc *var, expdesc *e);
LHATI_FUNC void lhatK_setreturns(FuncState *fs, expdesc *e, int nresults);
LHATI_FUNC void lhatK_setoneret(FuncState *fs, expdesc *e);
LHATI_FUNC int lhatK_jump(FuncState *fs);
LHATI_FUNC void lhatK_ret(FuncState *fs, int first, int nret);
LHATI_FUNC void lhatK_patchlist(FuncState *fs, int list, int target);
LHATI_FUNC void lhatK_patchtohere(FuncState *fs, int list);
LHATI_FUNC void lhatK_patchclose(FuncState *fs, int list, int level);
LHATI_FUNC void lhatK_concat(FuncState *fs, int *l1, int l2);
LHATI_FUNC int lhatK_getlabel(FuncState *fs);
LHATI_FUNC void lhatK_prefix(FuncState *fs, UnOpr op, expdesc *v, int line);
LHATI_FUNC void lhatK_infix(FuncState *fs, BinOpr op, expdesc *v);
LHATI_FUNC void lhatK_posfix(FuncState *fs, BinOpr op, expdesc *v1, expdesc *v2, int line);
LHATI_FUNC void lhatK_setlist(FuncState *fs, int base, int nelems, int tostore);

#endif