#ifndef lctype_h
#define lctype_h
//
// 'ctype' functions for L^
// See Copyright Notice in lhat.h
//

#include "lhat.h"

//
// WARNING: the functions defined here do not necessarily correspond
// to the similar functions in the standard C ctype.h. They are
// optimized for the specific needs of L^
//

#if !defined(LHAT_USE_CTYPE)
# if 'A' == 65 && '0' == 48
// ASCII case: can use its own tables; faster and fixed
#  define LHAT_USE_CTYPE	0
# else
// must use standard C ctype
#  define LHAT_USE_CTYPE	1
# endif
#endif


#if !LHAT_USE_CTYPE
# include <limits.h>
# include <stdbool.h>
# include "llimits.h"

enum {
	L_ALPHABIT  = 0,
	L_DIGITBIT  = 1,
	L_PRINTBIT  = 2,
	L_SPACEBIT  = 3,
	L_XDIGITBIT = 4,
};

// two more entries for 0 and -1 (EOZ)
LHATI_DDEC const lu_byte lhati_ctype_[UCHAR_MAX + 2];

//
// 'lalpha' (Lhat alphabetic) and 'lalnum' (Lhat alphanumeric) both include '_'
//
inline bool lislalpha(int c)
{
	return lhati_ctype_[c + 1] & (1 << L_ALPHABIT);
}
inline bool lislalnum(int c)
{
	return lhati_ctype_[c + 1] & ((1 << L_ALPHABIT) | (1 << L_DIGITBIT));
}
inline bool lisdigit(int c)
{
	return lhati_ctype_[c + 1] & (1 << L_DIGITBIT);
}
inline bool lisspace(int c)
{
	return lhati_ctype_[c + 1] & (1 << L_SPACEBIT);
}
inline bool lisprint(int c)
{
	return lhati_ctype_[c + 1] & (1 << L_PRINTBIT);
}
inline bool lisxdigit(int c)
{
	return lhati_ctype_[c + 1] & (1 << L_XDIGITBIT);
}

//
// this 'ltolower' only works for alphabetic characters
//
inline int ltolower(int c)
{
	return (c | ('A' ^ 'a'));
}

#else
//
// use standard C ctypes
//
# include <ctype.h>
inline bool lislalpha(int c)
{
	return isalpha(c) || (c) == '_';
}
inline bool lislalnum(int c)
{
	return isalnum(c) || (c) == '_';
}
inline bool lisdigit(int c)
{
	return isdigit(c);
}
inline bool lisspace(int c)
{
	return isspace(c);
}
inline bool lisprint(int c)
{
	return isprint(c);
}
inline bool lisxdigit(int c)
{
	return isxdigit(c);
}
inline bool ltolower(int c)
{
	return tolower(c);
}
#endif

#endif // !lctype_h