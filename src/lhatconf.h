#ifndef lhatconf_h
#define lhatconf_h
//
// Configuration file for L^
// See Copyright Notice in lhat.h
//

#include <limits.h>
#include <stddef.h>

//
// ===================================================================
// Search for "@@" to find all configurable definitions.
// ===================================================================
//


//
// {====================================================================
// System Configuration: macros to adapt (if needed) L^ to some
// particular platform, for instance compiling it with 32-bit numbers or
// restricting it to C89.
// =====================================================================
//

//
////@@ LHAT_32BITS enables L^ with 32-bit integers and 32-bit floats. You
// can also define LHAT_32BITS in the make file, but changing here you
// ensure that all software connected to L^ will be compiled with the
// same configuration.
//
// #define LHAT_32BITS


//
////@@ LHAT_USE_C89 controls the use of non-ISO-C89 features.
// Define it if you want L^ to avoid the use of a few C99 features
// or Windows-specific features on Windows.
//
// #define LHAT_USE_C89


//
// By default, L^ on Windows use (some) specific Windows features
//
#if !defined(LHAT_USE_C89) && defined(_WIN32) && !defined(_WIN32_WCE)
#define LHAT_USE_WINDOWS  // enable goodies for regular Windows
#endif


#if defined(LHAT_USE_WINDOWS)
#define LHAT_DL_DLL	// enable support for DLL
#define LHAT_BUILD_AS_DLL
//#define LHAT_USE_C89	// broadly, Windows is C89
#endif


#if defined(LHAT_USE_LINUX)
#define LHAT_USE_POSIX
#define LHAT_USE_DLOPEN		// needs an extra library: -ldl
#define LHAT_USE_READLINE	// needs some extra libraries
#endif


#if defined(LHAT_USE_MACOSX)
#define LHAT_USE_POSIX
#define LHAT_USE_DLOPEN		// MacOS does not need -ldl
#define LHAT_USE_READLINE	// needs an extra library: -lreadline
#endif


//
////@@ LHAT_C89_NUMBERS ensures that L^ uses the largest types available for
// C89 ('long' and 'double'); Windows always has '__int64', so it does
// not need to use this case.
//
#if defined(LHAT_USE_C89) && !defined(LHAT_USE_WINDOWS)
#define LHAT_C89_NUMBERS
#endif



//
////@@ LHATI_BITSINT defines the (minimum) number of bits in an 'int'.
//
// avoid undefined shifts
#if ((INT_MAX >> 15) >> 15) >= 1
#define LHATI_BITSINT	32
#else
// 'int' always must have at least 16 bits
#define LHATI_BITSINT	16
#endif


//
////@@ LHAT_INT_TYPE defines the type for L^ integers.
////@@ LHAT_FLOAT_TYPE defines the type for L^ floats.
// L^ should work fine with any mix of these options (if supported
// by your C compiler). The usual configurations are 64-bit integers
// and 'double' (the default), 32-bit integers and 'float' (for
// restricted platforms), and 'long'/'double' (for C compilers not
// compliant with C99, which may not have support for 'long long').
//

// predefined options for LHAT_INT_TYPE
#define LHAT_INT_INT		1
#define LHAT_INT_LONG		2
#define LHAT_INT_LONGLONG	3

// predefined options for LHAT_FLOAT_TYPE
#define LHAT_FLOAT_FLOAT		1
#define LHAT_FLOAT_DOUBLE	2
#define LHAT_FLOAT_LONGDOUBLE	3

#if defined(LHAT_32BITS)		// {
//
// 32-bit integers and 'float'
//
#if LHATI_BITSINT >= 32  // use 'int' if big enough
#define LHAT_INT_TYPE	LHAT_INT_INT
#else  // otherwise use 'long'
#define LHAT_INT_TYPE	LHAT_INT_LONG
#endif
#define LHAT_FLOAT_TYPE	LHAT_FLOAT_FLOAT

#elif defined(LHAT_C89_NUMBERS)	// }{
//
// largest types available for C89 ('long' and 'double')
//
#define LHAT_INT_TYPE	LHAT_INT_LONG
#define LHAT_FLOAT_TYPE	LHAT_FLOAT_DOUBLE

#endif				// }


//
// default configuration for 64-bit L^ ('long long' and 'double')
//
#if !defined(LHAT_INT_TYPE)
#define LHAT_INT_TYPE	LHAT_INT_LONGLONG
#endif

#if !defined(LHAT_FLOAT_TYPE)
#define LHAT_FLOAT_TYPE	LHAT_FLOAT_DOUBLE
#endif

// }==================================================================




//
// {==================================================================
// Configuration for Paths.
// ===================================================================
//

//
// LHAT_PATH_SEP is the character that separates templates in a path.
// LHAT_PATH_MARK is the string that marks the substitution points in a
// template.
// LHAT_EXEC_DIR in a Windows path is replaced by the executable's
// directory.
//
#define LHAT_PATH_SEP            ";"
#define LHAT_PATH_MARK           "?"
#define LHAT_EXEC_DIR            "!"


//
//@@ LHAT_PATH_DEFAULT is the default path that L^ uses to look for
// L^ libraries.
//@@ LHAT_CPATH_DEFAULT is the default path that L^ uses to look for
// C libraries.
// CHANGE them if your machine has a non-conventional directory
// hierarchy or if you want to install your libraries in
// non-conventional directories.
//
#define LHAT_VDIR	LHAT_VERSION_MAJOR_ "." LHAT_VERSION_MINOR_
#if defined(_WIN32)	// {
//
// In Windows, any exclamation mark ('!') in the path is replaced by the
// path of the directory of the executable file of the current process.
//
#define LHAT_LDIR	"!\\lhat\\"
#define LHAT_CDIR	"!\\"
#define LHAT_SHRDIR	"!\\..\\share\\lhat\\" LHAT_VDIR "\\"
#define LHAT_PATH_DEFAULT  \
		LHAT_LDIR"?.lhat;"  LHAT_LDIR"?\\init.lhat;" \
		LHAT_CDIR"?.lhat;"  LHAT_CDIR"?\\init.lhat;" \
		LHAT_SHRDIR"?.lhat;" LHAT_SHRDIR"?\\init.lhat;" \
		".\\?.lhat;" ".\\?\\init.lhat"
#define LHAT_CPATH_DEFAULT \
		LHAT_CDIR"?.dll;" \
		LHAT_CDIR"..\\lib\\lhat\\" LHAT_VDIR "\\?.dll;" \
		LHAT_CDIR"loadall.dll;" ".\\?.dll"

#else			// }{

#define LHAT_ROOT	"/usr/local/"
#define LHAT_LDIR	LHAT_ROOT "share/lhat/" LHAT_VDIR "/"
#define LHAT_CDIR	LHAT_ROOT "lib/lhat/" LHAT_VDIR "/"
#define LHAT_PATH_DEFAULT  \
		LHAT_LDIR"?.lhat;"  LHAT_LDIR"?/init.lhat;" \
		LHAT_CDIR"?.lhat;"  LHAT_CDIR"?/init.lhat;" \
		"./?.lhat;" "./?/init.lhat"
#define LHAT_CPATH_DEFAULT \
		LHAT_CDIR"?.so;" LHAT_CDIR"loadall.so;" "./?.so"
#endif			// }


//
//@@ LHAT_DIRSEP is the directory separator (for submodules).
// CHANGE it if your machine does not use "/" as the directory separator
// and is not Windows. (On Windows L^ automatically uses "\".)
//
#if defined(_WIN32)
#define LHAT_DIRSEP	"\\"
#else
#define LHAT_DIRSEP	"/"
#endif

// }==================================================================


//
// {==================================================================
// Marks for exported symbols in the C code
// ===================================================================
//

//
//@@ LHAT_API is a mark for all core API functions.
//@@ LHATLIB_API is a mark for all auxiliary library functions.
//@@ LHATMOD_API is a mark for all standard library opening functions.
// CHANGE them if you need to define those functions in some special way.
// For instance, if you want to create one Windows DLL with the core and
// the libraries, you may want to use the following definition (define
// LHAT_BUILD_AS_DLL to get it).
//
#if defined(LHAT_BUILD_AS_DLL)
# if defined(LHAT_CORE) || defined(LHAT_LIB)
#  define LHAT_API __declspec(dllexport)
# else
#  define LHAT_API __declspec(dllimport)
# endif
#else
# define LHAT_API		extern
#endif

// more often than not the libs go together with the core
#define LHATLIB_API	LHAT_API

#if defined(LHAT_BUILD_AS_DLL)
# define LHATMOD_DEC __declspec(dllimport)
# define LHATMOD_API __declspec(dllexport)
#else
# define LHATMOD_DEC		extern
# define LHATMOD_API		extern
#endif


//
//@@ LHATI_FUNC is a mark for all extern functions that are not to be
// exported to outside modules.
//@@ LHATI_DDEF and LHATI_DDEC are marks for all extern (const) variables
// that are not to be exported to outside modules (LHATI_DDEF for
// definitions and LHATI_DDEC for declarations).
// CHANGE them if you need to mark them in some special way. Elf/gcc
// (versions 3.2 and later) mark them as "hidden" to optimize access
// when L^ is compiled as a shared library. Not all elf targets support
// this attribute. Unfortunately, gcc does not offer a way to check
// whether the target offers that support, and those without support
// give a warning about it. To avoid these warnings, change to the
// default definition.
//
#if defined(__GNUC__) && ((__GNUC__*100 + __GNUC_MINOR__) >= 302) && defined(__ELF__)
# define LHATI_FUNC	__attribute__((visibility("hidden"))) extern
#else
# define LHATI_FUNC	extern
#endif

#define LHATI_DDEC	LHATI_FUNC
#define LHATI_DDEF	// empty

// }==================================================================

//
// {==================================================================
// Configuration for Numbers.
// Change these definitions if no predefined LHAT_FLOAT_* / LHAT_INT_*
// satisfy your needs.
// ===================================================================
//

//
//@@ LHAT_NUMBER is the floating-point type used by L^.
//@@ LHATI_UACNUMBER is the result of a 'default argument promotion'
//@@ over a floating number.
//@@ l_mathlim(x) corrects limit name 'x' to the proper float type
// by prefixing it with one of FLT/DBL/LDBL.
//@@ LHAT_NUMBER_FRMLEN is the length modifier for writing floats.
//@@ LHAT_NUMBER_FMT is the format for writing floats.
//@@ lhat_number2str converts a float to a string.
//@@ l_mathop allows the addition of an 'l' or 'f' to all math operations.
//@@ l_floor takes the floor of a float.
//@@ lhat_str2number converts a decimal numeric string to a number.
//


// The following definitions are good for most cases here

#define l_floor(x)		(l_mathop(floor)(x))

#define lhat_number2str(s,sz,n)  \
	l_sprintf((s), sz, LHAT_NUMBER_FMT, (LHATI_UACNUMBER)(n))

//
//@@ lhat_numbertointeger converts a float number to an integer, or
// returns 0 if float is not within the range of a lhat_Integer.
// (The range comparisons are tricky because of rounding. The tests
// here assume a two-complement representation, where MININTEGER always
// has an exact representation as a float; MAXINTEGER may not have one,
// and therefore its conversion to float may have an ill-defined value.)
//
#define lhat_numbertointeger(n,p) \
  ((n) >= (LHAT_NUMBER)(LHAT_MININTEGER) && \
   (n) < -(LHAT_NUMBER)(LHAT_MININTEGER) && \
      (*(p) = (LHAT_INTEGER)(n), 1))


// now the variable definitions

#if LHAT_FLOAT_TYPE == LHAT_FLOAT_FLOAT		// { single float

#define LHAT_NUMBER	float

#define l_mathlim(n)		(FLT_##n)

#define LHATI_UACNUMBER	double

#define LHAT_NUMBER_FRMLEN	""
#define LHAT_NUMBER_FMT		"%.7g"

#define l_mathop(op)		op##f

#define lhat_str2number(s,p)	strtof((s), (p))


#elif LHAT_FLOAT_TYPE == LHAT_FLOAT_LONGDOUBLE	// }{ long double

#define LHAT_NUMBER	long double

#define l_mathlim(n)		(LDBL_##n)

#define LHATI_UACNUMBER	long double

#define LHAT_NUMBER_FRMLEN	"L"
#define LHAT_NUMBER_FMT		"%.19Lg"

#define l_mathop(op)		op##l

#define lhat_str2number(s,p)	strtold((s), (p))

#elif LHAT_FLOAT_TYPE == LHAT_FLOAT_DOUBLE	// }{ double

#define LHAT_NUMBER	double

#define l_mathlim(n)		(DBL_##n)

#define LHATI_UACNUMBER	double

#define LHAT_NUMBER_FRMLEN	""
#define LHAT_NUMBER_FMT		"%.14g"

#define l_mathop(op)		op

#define lhat_str2number(s,p)	strtod((s), (p))

#else						// }{

#error "numeric float type not defined"

#endif					// }



//
//@@ LHAT_INTEGER is the integer type used by L^.
//
//@@ LHAT_UNSIGNED is the unsigned version of LHAT_INTEGER.
//
//@@ LHATI_UACINT is the result of a 'default argument promotion'
//@@ over a lUA_INTEGER.
//@@ LHAT_INTEGER_FRMLEN is the length modifier for reading/writing integers.
//@@ LHAT_INTEGER_FMT is the format for writing integers.
//@@ LHAT_MAXINTEGER is the maximum value for a LHAT_INTEGER.
//@@ LHAT_MININTEGER is the minimum value for a LHAT_INTEGER.
//@@ lhat_integer2str converts an integer to a string.
//


// The following definitions are good for most cases here

#define LHAT_INTEGER_FMT		"%" LHAT_INTEGER_FRMLEN "d"

#define LHATI_UACINT		LHAT_INTEGER

#define lhat_integer2str(s,sz,n)  \
	l_sprintf((s), sz, LHAT_INTEGER_FMT, (LHATI_UACINT)(n))

//
// use LHATI_UACINT here to avoid problems with promotions (which
// can turn a comparison between unsigneds into a signed comparison)
//
#define LHAT_UNSIGNED		unsigned LHATI_UACINT


// now the variable definitions

#if LHAT_INT_TYPE == LHAT_INT_INT		// { int

#define LHAT_INTEGER		int
#define LHAT_INTEGER_FRMLEN	""

#define LHAT_MAXINTEGER		INT_MAX
#define LHAT_MININTEGER		INT_MIN

#elif LHAT_INT_TYPE == LHAT_INT_LONG	// }{ long

#define LHAT_INTEGER		long
#define LHAT_INTEGER_FRMLEN	"l"

#define LHAT_MAXINTEGER		LONG_MAX
#define LHAT_MININTEGER		LONG_MIN

#elif LHAT_INT_TYPE == LHAT_INT_LONGLONG	// }{ long long

// use presence of macro LLONG_MAX as proxy for C99 compliance
#if defined(LLONG_MAX)		// {
// use ISO C99 stuff

#define LHAT_INTEGER		long long
#define LHAT_INTEGER_FRMLEN	"ll"

#define LHAT_MAXINTEGER		LLONG_MAX
#define LHAT_MININTEGER		LLONG_MIN

#elif defined(LHAT_USE_WINDOWS) // }{
// in Windows, can use specific Windows types

#define LHAT_INTEGER		__int64
#define LHAT_INTEGER_FRMLEN	"I64"

#define LHAT_MAXINTEGER		_I64_MAX
#define LHAT_MININTEGER		_I64_MIN

#else				// }{

#error "Compiler does not support 'long long'. Use option '-DLHAT_32BITS' \
  or '-DLHAT_C89_NUMBERS' (see file 'lhatconf.h' for details)"

#endif				// }

#else				// }{

#error "numeric integer type not defined"

#endif				// }

// }==================================================================


//
// {==================================================================
// Dependencies with C99 and other C details
// ===================================================================
//

//
//@@ l_sprintf is equivalent to 'snprintf' or 'sprintf' in C89.
// (All uses in L^ have only one format item.)
//
#if !defined(LHAT_USE_C89)
# define l_sprintf(s,sz,f,i)	snprintf(s,sz,f,i)
#else
# define l_sprintf(s,sz,f,i)	((void)(sz), sprintf(s,f,i))
#endif


//
//@@ lhat_strx2number converts an hexadecimal numeric string to a number.
// In C99, 'strtod' does that conversion. Otherwise, you can
// leave 'lhat_strx2number' undefined and L^ will provide its own
// implementation.
//
#if !defined(LHAT_USE_C89)
# define lhat_strx2number(s,p)		lhat_str2number(s,p)
#endif


//
//@@ lhat_number2strx converts a float to an hexadecimal numeric string.
// In C99, 'sprintf' (with format specifiers '%a'/'%A') does that.
// Otherwise, you can leave 'lhat_number2strx' undefined and L^ will
// provide its own implementation.
//
#if !defined(LHAT_USE_C89)
#define lhat_number2strx(L,b,sz,f,n)  \
	((void)L, l_sprintf(b,sz,f,(LHATI_UACNUMBER)(n)))
#endif


//
// 'strtof' and 'opf' variants for math functions are not valid in
// C89. Otherwise, the macro 'HUGE_VALF' is a good proxy for testing the
// availability of these variants. ('math.h' is already included in
// all files that use these macros.)
//
#if defined(LHAT_USE_C89) || (defined(HUGE_VAL) && !defined(HUGE_VALF))
#undef l_mathop  // variants not available
#undef lhat_str2number
#define l_mathop(op)		(lhat_Number)op  // no variant
#define lhat_str2number(s,p)	((lhat_Number)strtod((s), (p)))
#endif


//
//@@ LHAT_KCONTEXT is the type of the context ('ctx') for continuation
// functions.  It must be a numerical type; L^ will use 'intptr_t' if
// available, otherwise it will use 'ptrdiff_t' (the nearest thing to
// 'intptr_t' in C89)
//
#define LHAT_KCONTEXT	ptrdiff_t

#if !defined(LHAT_USE_C89) && defined(__STDC_VERSION__) &&  __STDC_VERSION__ >= 199901L
# include <stdint.h>
# if defined(INTPTR_MAX)  // even in C99 this type is optional
#  undef LHAT_KCONTEXT
#  define LHAT_KCONTEXT	intptr_t
# endif
#endif


//
//@@ lhat_getlocaledecpoint gets the locale "radix character" (decimal point).
// Change that if you do not want to use C locales. (Code using this
// macro must include header 'locale.h'.)
//
#if !defined(lhat_getlocaledecpoint)
# define lhat_getlocaledecpoint()		(localeconv()->decimal_point[0])
#endif

// }==================================================================


//
// {==================================================================
// Language Variations
// =====================================================================
//

//
//@@ LHAT_NOCVTN2S/LHAT_NOCVTS2N control how L^ performs some
// coercions. Define LHAT_NOCVTN2S to turn off automatic coercion from
// numbers to strings. Define LHAT_NOCVTS2N to turn off automatic
// coercion from strings to numbers.
//
// #define LHAT_NOCVTN2S
// #define LHAT_NOCVTS2N


//
//@@ LHAT_USE_APICHECK turns on several consistency checks on the C API.
// Define it as a help when debugging C code.
//
#if defined(LHAT_USE_APICHECK)
# include <assert.h>
# define lhati_apicheck(l,e)	assert(e)
#endif

// }==================================================================


//
// {==================================================================
// Macros that affect the API and must be stable (that is, must be the
// same when you compile L^ and when you compile code that links to
// L^). You probably do not want/need to change them.
// =====================================================================
//

//
//@@ LHATI_MAXSTACK limits the size of the L^ stack.
// CHANGE it if you need a different limit. This limit is arbitrary;
// its only purpose is to stop L^ from consuming unlimited stack
// space (and to reserve some numbers for pseudo-indices).
//
#if LHATI_BITSINT >= 32
#define LHATI_MAXSTACK		1000000
#else
#define LHATI_MAXSTACK		15000
#endif


//
//@@ LHAT_EXTRASPACE defines the size of a raw memory area associated with
// a L^ state with very fast access.
// CHANGE it if you need a different size.
//
#define LHAT_EXTRASPACE		(sizeof(void *))


//
//@@ LHAT_IDSIZE gives the maximum size for the description of the source
//@@ of a function in debug information.
// CHANGE it if you want a different size.
//
#define LHAT_IDSIZE	60


//
//@@ LHATL_BUFFERSIZE is the buffer size used by the lauxlib buffer system.
// CHANGE it if it uses too much C-stack space. (For long double,
// 'string.format("%.99f", -1e4932)' needs 5034 bytes, so a
// smaller buffer would force a memory allocation for each call to
// 'string.format'.)
//
#if LHAT_FLOAT_TYPE == LHAT_FLOAT_LONGDOUBLE
# define LHATL_BUFFERSIZE		8192
#else
# define LHATL_BUFFERSIZE   ((int)(0x80 * sizeof(void*) * sizeof(lhat_Integer)))
#endif

// }==================================================================


//
//@@ LHAT_QL describes how error messages quote program elements.
// L^ does not use these macros anymore; they are here for
// compatibility only.
//
#define LHAT_QL(x)	"'" x "'"
#define LHAT_QS		LHAT_QL("%s")




// ===================================================================

//
// Local configuration. You can use this space to add your redefinitions
// without modifying the main part of the file.
//

#endif