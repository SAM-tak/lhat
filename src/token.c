// L^ (lhat) -- token name tables, used by diagnostics and tests.

#include "token.h"

const char *lhat_token_kind_name(LhatTokenKind kind)
{
    switch (kind) {
        case LHAT_TOKEN_EOF:         return "eof";
        case LHAT_TOKEN_IDENT:       return "ident";
        case LHAT_TOKEN_HAT_IDENT:   return "hat-ident";
        case LHAT_TOKEN_NAME_LITERAL: return "name-literal";
        case LHAT_TOKEN_INT:         return "int";
        case LHAT_TOKEN_FLOAT:       return "float";
        case LHAT_TOKEN_STRING:      return "string";
        case LHAT_TOKEN_SCOPE:       return "scope";
        case LHAT_TOKEN_OP:          return "op";

        case LHAT_TOKEN_INTERP_BEGIN:      return "interp-begin";
        case LHAT_TOKEN_INTERP_TEXT:       return "interp-text";
        case LHAT_TOKEN_INTERP_EXPR_BEGIN: return "interp-expr-begin";
        case LHAT_TOKEN_INTERP_FORMAT:     return "interp-format";
        case LHAT_TOKEN_INTERP_EXPR_END:   return "interp-expr-end";
        case LHAT_TOKEN_INTERP_END:        return "interp-end";

        case LHAT_TOKEN_ERROR:       return "error";
    }
    return "?";
}

const char *lhat_op_name(LhatOpKind op)
{
    switch (op) {
        case LHAT_OP_LPAREN:     return "(";
        case LHAT_OP_RPAREN:     return ")";
        case LHAT_OP_LBRACKET:   return "[";
        case LHAT_OP_RBRACKET:   return "]";
        case LHAT_OP_LBRACE:     return "{";
        case LHAT_OP_RBRACE:     return "}";
        case LHAT_OP_COMMA:      return ",";
        case LHAT_OP_SEMICOLON:  return ";";
        case LHAT_OP_COLON:      return ":";
        case LHAT_OP_DEFINE:     return ":=";
        case LHAT_OP_REASSIGN:   return "<<";
        case LHAT_OP_COLONCOLON: return "::";
        case LHAT_OP_ARROW:      return "->";
        case LHAT_OP_EQ:         return "=";
        case LHAT_OP_NE:         return "!=";
        case LHAT_OP_LE:         return "<=";
        case LHAT_OP_GE:         return ">=";
        case LHAT_OP_LT:         return "<";
        case LHAT_OP_GT:         return ">";
        case LHAT_OP_ADD:        return "+";
        case LHAT_OP_SUB:        return "-";
        case LHAT_OP_MUL:        return "*";
        case LHAT_OP_DIV:        return "/";
        case LHAT_OP_FLOORDIV:   return "//";
        case LHAT_OP_MOD:        return "%";
        case LHAT_OP_POW:        return "**";
        case LHAT_OP_NOT:        return "!";
        case LHAT_OP_AND:        return "and^";
        case LHAT_OP_OR:         return "or^";
        case LHAT_OP_IS:         return "is^";
        case LHAT_OP_CATCH:      return "catch^";
        case LHAT_OP_UNION:      return "|";
        case LHAT_OP_INTERSECT:  return "&";
        case LHAT_OP_DOT:        return ".";
        case LHAT_OP_CONCAT:     return "..";
        case LHAT_OP_ELLIPSIS:   return "...";
        case LHAT_OP_NIL_DOT:    return "?.";
        case LHAT_OP_NIL_CALL:   return "?(";
        case LHAT_OP_NIL_INDEX:  return "?[";
        case LHAT_OP_NIL_ELSE:   return "??";
        case LHAT_OP_AT:         return "@";
        case LHAT_OP_COUNT:      break;
    }
    return "?";
}

const char *lhat_string_kind_name(LhatStringKind kind)
{
    switch (kind) {
        case LHAT_STRING_ESCAPED: return "escaped";
        case LHAT_STRING_RAW:     return "raw";
        case LHAT_STRING_LINE:    return "line";
    }
    return "?";
}

const char *lhat_scope_kind_name(LhatScopeKind kind)
{
    switch (kind) {
        case LHAT_SCOPE_GLOBAL:   return "global";
        case LHAT_SCOPE_FILE:     return "file";
        case LHAT_SCOPE_RELATIVE: return "relative";
    }
    return "?";
}
