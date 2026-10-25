// SPDX-License-Identifier: GPL-3.0-or-later

// msys2's mingw-w64 CRT r532 ships libmingwex's fesetenv.o referencing
// __mingw_fe_dfl_env but no longer archives the definition object, which
// breaks -static links of the audio-hook DLL (and anything else that pulls
// fesetenv in transitively). Supply the default floating-point environment
// locally. A zero-initialized fenv_t matches FE_DFL_ENV on x86_64 (all
// exception flags clear, default rounding mode) for these code paths.
#include <fenv.h>

extern "C" const fenv_t __mingw_fe_dfl_env = {};
