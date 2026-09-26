// Private bridge for math.quaternion: the Vector3 tag is shared per process.
// The caller must register std.math.vector3 before std.math.quaternion.
#ifndef LHATSTDLIB_MATHVECTOR3_INTERNAL_H
#define LHATSTDLIB_MATHVECTOR3_INTERNAL_H

#include "lhat.h"

const LhatHostValueTag *lhatstdlib_mathvector3_tag(void);

#endif  // LHATSTDLIB_MATHVECTOR3_INTERNAL_H
