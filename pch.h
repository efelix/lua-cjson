// pch.h: это предварительно скомпилированный заголовочный файл.
// Перечисленные ниже файлы компилируются только один раз, что ускоряет последующие сборки.
// Это также влияет на работу IntelliSense, включая многие функции просмотра и завершения кода.
// Однако изменение любого из приведенных здесь файлов между операциями сборки приведет к повторной компиляции всех(!) этих файлов.
// Не добавляйте сюда файлы, которые планируете часто изменять, так как в этом случае выигрыша в производительности не будет.

#ifndef PCH_H
#define PCH_H
// Добавьте сюда заголовочные файлы для предварительной компиляции

#include "framework.h"
#include <Windows.h>

#ifdef _WIN32
#define inline __inline
#define snprintf_s _snprintf_s
#endif


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <stdarg.h>
#include <string.h>
#include <locale.h>
#include <math.h>

#ifdef _MSC_VER
/* Microsoft C compiler lacks strncasecmp and strcasecmp. */
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#endif


#define LUA_LIB
#define LUA_BUILD_AS_DLL

#ifdef __cplusplus
#include "lua\lua.hpp"
#else
#include "lua\lua.h"
#endif

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "lua\luaconf.h"
#include "lua\lauxlib.h"

#ifdef __cplusplus
}
#endif

#define checkStack(L) luaL_checkstack(L, LUA_MINSTACK, "too many nested values")

#if LUA_VERSION_NUM < 502
#define lua_rawlen(L, idx) lua_objlen(L, idx)
#endif

#endif //PCH_H
