// L^ (lhat) -- sample standard library: std.json.
//
// Two calls, and a mechanical rule between them.
//
//   std.json.encode : f^t^{} -> string^|std.json.JsonError|std.error.OutOfMemory;
//   std.json.decode : f^string^ -> t^{}|std.json.JsonError|std.error.OutOfMemory;
//
// 14 章 gives L^ one table that serves as both a sequence and a mapping,
// and JSON has two shapes for that, so a rule has to say which. It is read
// off the halves of the table itself:
//
//   only the dense part, and not empty  ->  an array
//   anything else, empty included       ->  an object
//
// So `{1, 2, 3}` writes as `[1,2,3]`, `{a := 1}` as `{"a":1}`, and one
// holding both writes as an object whose dense half took the keys "1"…"n".
// The empty table has no half to read, and is written `{}`: an empty table
// is more often a record about to be filled than a list about to be.
//
// What crosses, and how:
//
//   nil^                     null
//   bool^                    true / false
//   an integer               a number with no fraction
//   a real                   a number written so that reading it back
//                            answers the same real. NaN and the infinities
//                            have no spelling in JSON: Unsupported
//   string^                  a quoted string. The bytes have to be UTF-8,
//                            since a JSON string is text: Unsupported if not
//   a table                  as above
//   anything else            Unsupported -- a closure, a coroutine, an
//                            error, a hostdata or host value, a box
//
// An object's keys are its string keys as they are, and its integer keys
// spelt out. A key of any other kind is Unsupported.
//
// **An object's keys are written in order**, by the key as it is spelt --
// so "10" stands before "2". 02 の 14.16 answered the same question the same
// way and for the same reason: a table's own order is the hash's, which is
// neither the writer's nor stable, so what is written is put in a canonical
// order instead. It follows that two equal tables write the same text, and
// that a decode and a second encode answer the text they started from.
//
// 04 の 11.3 spells "not there" nil^, and that has two sides here.
//
// Reading: JSON's null puts no key. So `[1, null, 3]` answers a table with
// nothing at 2 -- and encoding that back answers an object, since the table
// is no longer only a dense part. This is the one place the round trip does
// not close, and it closes as far as 11.3 lets it.
//
// Writing: storing nil^ in a table is how a key goes, so **a table never
// holds one** and an encode never writes a null. The row above is what a
// nil^ would become and not something a table can be made to carry.
//
// Nesting deeper than the reader or the writer will follow answers TooDeep.
// A table holding itself reaches that too: the depth is the whole of what
// is counted, rather than a second pass looking for one.
//
// The top of a decode has to be an object or an array. RFC 8259 lets a
// scalar stand alone, but the two calls here are about tables, and a
// signature that says so is worth more than the one text it turns away.
//
// What decode answers is `t^{}` -- a table promising nothing, because
// nothing about the text was known when the program was checked. So its
// contents are read by index and not by name:
//
//   let^ back = try^ std.json.decode(text)
//   print(back["name"])          # and not back.name, which 03 の 3.1 refuses
//
// A program that knows the shape can say so and narrow to it, which is
// where the type it wanted comes back.

#ifndef LHATSTDLIB_JSON_H
#define LHATSTDLIB_JSON_H

#include <stdbool.h>

#include "lhat.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lhatstdlib_json_register(LhatProgram *program);

#ifdef __cplusplus
}
#endif

#endif  // LHATSTDLIB_JSON_H
