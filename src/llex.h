#ifndef llex_h
#define llex_h
//
// Lexical Analyzer
// See Copyright Notice in lhat.h
//

#include "lobject.h"
#include "lzbuf.h"

#if !defined(LHAT_ENV)
# define LHAT_ENV		"_ENV"
#endif

//
// WARNING: if you change the order of this enumeration,
// grep "ORDER RESERVED"
//
enum RESERVED {
	FIRST_RESERVED = 257,
	// terminal symbols denoted by reserved words
	TK_AND = FIRST_RESERVED, TK_BREAK,
	TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
	TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
	TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
	// other terminal symbols
	TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
	TK_SHL, TK_SHR,
	TK_DBCOLON, TK_EOS,
	TK_FLT, TK_INT, TK_NAME, TK_STRING,
	// number of reserved words
	NUM_RESERVED = TK_WHILE - FIRST_RESERVED + 1
};

// semantics information
typedef union {
	lhat_Number r;
	lhat_Integer i;
	TString *ts;
} SemInfo;  

typedef struct Token {
	int token;
	SemInfo seminfo;
} Token;

// state of the lexer plus state of the parser when shared by all functions
typedef struct LexState {
	int current;  // current character (charint)
	int linenumber;  // input line counter
	int lastline;  // line of last token 'consumed'
	Token t;  // current token
	Token lookahead;  // look ahead token
	struct FuncState *fs;  // current function (parser)
	struct lhat_State *L;
	ZBuf *z;  // input stream
	Mbuffer *buff;  // buffer for tokens
	Table *h;  // to avoid collection/reuse strings
	struct Dyndata *dyd;  // dynamic structures used by the parser
	TString *source;  // current source name
	TString *envn;  // environment variable name
} LexState;

LHATI_FUNC void lhatX_init(lhat_State *L);
LHATI_FUNC void lhatX_setinput(lhat_State *L, LexState *ls, ZBuf *z, TString *source, int firstchar);
LHATI_FUNC TString *lhatX_newstring(LexState *ls, const char *str, size_t l);
LHATI_FUNC void lhatX_next(LexState *ls);
LHATI_FUNC int lhatX_lookahead(LexState *ls);
LHATI_FUNC l_noret lhatX_syntaxerror(LexState *ls, const char *s);
LHATI_FUNC const char *lhatX_token2str(LexState *ls, int token);

#endif