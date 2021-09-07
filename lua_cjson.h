#pragma once
#ifndef LUA_CJSON_H
#define LUA_CJSON_H
/*
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "pch.h"
#include "strbuf.h"

#ifndef CJSON_MODNAME
#define CJSON_MODNAME   "lua_cjson"
#endif

#ifndef CJSON_VERSION
#define CJSON_VERSION   "2.1.1.QUIKSHARP"
#endif

#ifdef _MSC_VER
#define snprintf sprintf_s
 /* Microsoft C compiler lacks strncasecmp and strcasecmp. */
#define strncasecmp _strnicmp
#define strcasecmp _stricmp

#ifndef isnan
#include <float.h>
#define isnan(x) _isnan(x)
#endif
#endif

 // convention is to use a pointer to anything in our C module for unique lightuserdata
#define CJSON_LIGHTUSERDATA ((void*)json_token_type_name)
#define CJSON_REGISTRY_INVALID_TYPE_ENCODER 1

/* Workaround for Solaris platforms missing isinf() */
#if !defined(isinf) && (defined(USE_INTERNAL_ISINF) || defined(MISSING_ISINF))
#define isinf(x) (!isnan(x) && isnan((x) - (x)))
#endif

#define DEFAULT_SPARSE_CONVERT 0
#define DEFAULT_SPARSE_RATIO 2
#define DEFAULT_SPARSE_SAFE 10
#define DEFAULT_ENCODE_MAX_DEPTH 1000
#define DEFAULT_DECODE_MAX_DEPTH 1000
#define DEFAULT_ENCODE_INVALID_NUMBERS 0
#define DEFAULT_DECODE_INVALID_NUMBERS 1
#define DEFAULT_ENCODE_KEEP_BUFFER 1
#define DEFAULT_ENCODE_NUMBER_PRECISION 14
#define DEFAULT_ENCODE_EMPTY_TABLE_AS_OBJECT 0
#define DEFAULT_ENCODE_ESCAPE_FORWARD_SLASH 1
#define DEFAULT_ENCODE_ESCAPE_UTF8 0
#define DEFAULT_ENCODE_NUMBERS_AS_BASE64 0

#if LONG_MAX > ((1UL << 31) - 1)
#define json_lightudata_mask(ludata)                                         \
    ((void *) ((uintptr_t) (ludata) & ((1UL << 47) - 1)))

#else
#define json_lightudata_mask(ludata)    (ludata)
#endif

#if LUA_VERSION_NUM > 501
#define lua_objlen(L,i)		lua_rawlen(L, (i))
#endif

typedef enum {
    T_OBJ_BEGIN,
    T_OBJ_END,
    T_ARR_BEGIN,
    T_ARR_END,
    T_STRING,
    T_NUMBER,
    T_INTEGER,
    T_BOOLEAN,
    T_NULL,
    T_COLON,
    T_COMMA,
    T_END,
    T_WHITESPACE,
    T_ERROR,
    T_UNKNOWN
} json_token_type_t;

static const char* json_token_type_name[] = {
    "T_OBJ_BEGIN",
    "T_OBJ_END",
    "T_ARR_BEGIN",
    "T_ARR_END",
    "T_STRING",
    "T_NUMBER",
    "T_INTEGER",
    "T_BOOLEAN",
    "T_NULL",
    "T_COLON",
    "T_COMMA",
    "T_END",
    "T_WHITESPACE",
    "T_ERROR",
    "T_UNKNOWN",
    NULL
};

typedef struct {
    json_token_type_t ch2token[256];
    char escape2char[256];  /* Decoding */

    /* encode_buf is only allocated and used when
     * encode_keep_buffer is set */
    strbuf_t encode_buf;

    int encode_sparse_convert;
    int encode_sparse_ratio;
    int encode_sparse_safe;
    int encode_max_depth;
    int encode_invalid_numbers;     /* 2 => Encode as "null" */
    int encode_number_precision;
    int encode_keep_buffer;
    int encode_empty_table_as_object;
    int encode_escape_forward_slash;
    int encode_escape_utf8;       /* 0, 1, -i -> char i */
    int decode_invalid_numbers;
    int decode_max_depth;
    int encode_numbers_as_base64;
    int decode_numbers_as_base64;
} json_config_t;

typedef struct {
    const char* data;
    const char* ptr;
    strbuf_t* tmp;    /* Temporary storage for strings */
    json_config_t* cfg;
    int current_depth;
} json_parse_t;

typedef struct {
    json_token_type_t type;
    int index;
    union {
        const char* string;
        double number;
        lua_Integer integer;
        int boolean;
    } value;
    int string_len;
} json_token_t;

#endif