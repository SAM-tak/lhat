#ifndef lhat_lprefix_h
#define lhat_lprefix_h
//
// Definitions for L^ code that must come before any other header file
// See Copyright Notice in lhat.h
//

//
// Allows POSIX/XSI stuff
//
#if !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE           600
#elif _XOPEN_SOURCE == 0
# undef _XOPEN_SOURCE  // use -D_XOPEN_SOURCE=0 to undefine it
#endif
//
// Allows manipulation of large files in gcc and some other compilers
//
#if !defined(LHAT_32BITS) && !defined(_FILE_OFFSET_BITS)
# define _LARGEFILE_SOURCE       1
# define _FILE_OFFSET_BITS       64
#endif


//
// Windows stuff
//
#if defined(_WIN32)
# if !defined(_CRT_SECURE_NO_WARNINGS)
#  define _CRT_SECURE_NO_WARNINGS  // avoid warnings about ISO C functions
# endif
#endif

#endif // !lhat_lprefix_h