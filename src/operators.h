// L^ (lhat) -- the operators a type may define.
//
// Not a lexical fact, which is why it is here rather than beside the tokens:
// 01 の 7 章 scans '+' and '<=>' the same way it scans '|' and '?', and this
// is the shorter list of the ones 02 の 11.8 lets op^ name. It expands to
// instruction names as well, so it would carry the machine into any header it
// sat in.

#ifndef LHAT_OPERATORS_H
#define LHAT_OPERATORS_H

// 02 の 11.8 with 11.9: the spellings op^ may write -- '..', the seven
// arithmetic operators, '<=>' and '='. The last is the one comparison
// written on its own: a type may know what equals what without knowing what
// comes first, and 11.9 has '=' and '≠' read it before reaching for '<=>'.
// The other four comparisons are read off '<=>' alone. One list, expanded
// three ways: the checker's member naming, the machine's candidate lookup,
// and the membership test. They have to agree byte for byte, and one list is
// what makes them. X(op, bc, spelling, length): `op` completes LHAT_OP_##op
// and `bc` LHAT_BC_##bc, named apart because '//' is FLOORDIV as a token and
// IDIV as an instruction.
#define LHAT_OPERATOR_MEMBERS(X) \
    X(SPACESHIP, SPACESHIP, "<=>", 3) \
    X(EQ,        EQ,        "=",   1) \
    X(CONCAT,    CONCAT,    "..",  2) \
    X(ADD,       ADD,       "+",   1) \
    X(SUB,       SUB,       "-",   1) \
    X(MUL,       MUL,       "*",   1) \
    X(DIV,       DIV,       "/",   1) \
    X(FLOORDIV,  IDIV,      "//",  2) \
    X(MOD,       MOD,       "%",   1) \
    X(POW,       POW,       "**",  2)

#endif  // LHAT_OPERATORS_H
