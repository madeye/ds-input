// Guids.cpp — the single translation unit that *defines* storage for the GUIDs.
//
// <initguid.h> must be included BEFORE Guids.h here. With <initguid.h> active,
// each DEFINE_GUID in Guids.h expands to a real `const GUID name = {...}`
// definition. In every other .cpp, Guids.h is included without <initguid.h>, so
// the same DEFINE_GUID lines expand to `extern const GUID name` declarations.
// Result: exactly one definition, shared by all TUs — no duplicate symbols.

#include <initguid.h>
#include "Guids.h"
