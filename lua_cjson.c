/* Lua CJSON - JSON support for Lua
 *
 * Copyright (c) 2010-2012  Mark Pulford <mark@kyne.com.au>
 *
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

/* Caveats:
 * - JSON "null" values are represented as lightuserdata since Lua
 *   tables cannot contain "nil". Compare with cjson.null.
 * - Invalid UTF-8 characters are not detected and will be passed
 *   untouched. If required, UTF-8 error checking should be done
 *   outside this library.
 * - Javascript comments are not part of the JSON spec, and are not
 *   currently supported.
 *
 * Note: Decoding is slower than encoding. Lua spends significant
 *       time (30%) managing tables when parsing JSON since it is
 *       difficult to know object/array sizes ahead of time.
 */

#include "pch.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include "lua_cjson.h"
#include "utf8_decoder.h"
#include "base64.h"
#include "itoa.h"
#include "atoluanumber.h"

static const char *char2escape[256] = {
    "\\u0000", "\\u0001", "\\u0002", "\\u0003",
    "\\u0004", "\\u0005", "\\u0006", "\\u0007",
    "\\b", "\\t", "\\n", "\\u000b",
    "\\f", "\\r", "\\u000e", "\\u000f",
    "\\u0010", "\\u0011", "\\u0012", "\\u0013",
    "\\u0014", "\\u0015", "\\u0016", "\\u0017",
    "\\u0018", "\\u0019", "\\u001a", "\\u001b",
    "\\u001c", "\\u001d", "\\u001e", "\\u001f",
    NULL, NULL, "\\\"", NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, "\\/",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, "\\\\", NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, "\\u007f",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

/* ===== CONFIGURATION ===== */

static json_config_t *json_fetch_config(lua_State *l)
{
    json_config_t *cfg;

    cfg = (json_config_t*)lua_touserdata(l, lua_upvalueindex(1));
    if (!cfg)
        luaL_error(l, "BUG: Unable to fetch CJSON configuration");

    return cfg;
}

/* Ensure the correct number of arguments have been provided.
 * Pad with nil to allow other functions to simply check arg[i]
 * to find whether an argument was provided */
static json_config_t *json_arg_init(lua_State *l, int args)
{
    luaL_argcheck(l, lua_gettop(l) <= args, args + 1,
                  "found too many arguments");

    while (lua_gettop(l) < args)
        lua_pushnil(l);

    return json_fetch_config(l);
}

/* Process integer options for configuration functions */
static int json_integer_option(lua_State *l, int optindex, int *setting,
                               int min, int max)
{
    char errmsg[64];
    int value;

    if (!lua_isnil(l, optindex)) {
        value = (int)luaL_checkinteger(l, optindex);
        snprintf(errmsg, sizeof(errmsg), "expected integer between %d and %d", min, max);
        luaL_argcheck(l, min <= value && value <= max, 1, errmsg);
        *setting = value;
    }

    lua_pushinteger(l, *setting);

    return 1;
}

/* Process enumerated arguments for a configuration function */
static int json_enum_option(lua_State *l, int optindex, int *setting,
                            const char **options, int bool_true)
{
    static const char *bool_options[] = { "off", "on", NULL };

    if (!options) {
        options = bool_options;
        bool_true = 1;
    }

    if (!lua_isnil(l, optindex)) {
        if (bool_true && lua_isboolean(l, optindex))
            *setting = lua_toboolean(l, optindex) * bool_true;
        else
            *setting = luaL_checkoption(l, optindex, NULL, options);
    }

    if (bool_true && (*setting == 0 || *setting == bool_true))
        lua_pushboolean(l, *setting);
    else
        lua_pushstring(l, options[*setting]);

    return 1;
}

/* Configures handling of extremely sparse arrays:
 * convert: Convert extremely sparse arrays into objects? Otherwise error.
 * ratio: 0: always allow sparse; 1: never allow sparse; >1: use ratio
 * safe: Always use an array when the max index <= safe */
static int json_cfg_encode_sparse_array(lua_State *l)
{
    json_config_t *cfg = json_arg_init(l, 3);

    json_enum_option(l, 1, &cfg->encode_sparse_convert, NULL, 1);
    json_integer_option(l, 2, &cfg->encode_sparse_ratio, 0, INT_MAX);
    json_integer_option(l, 3, &cfg->encode_sparse_safe, 0, INT_MAX);

    return 3;
}

/* Configures the maximum number of nested arrays/objects allowed when
 * encoding */
static int json_cfg_encode_max_depth(lua_State *l)
{
    json_config_t *cfg = json_arg_init(l, 1);

    return json_integer_option(l, 1, &cfg->encode_max_depth, 1, INT_MAX);
}

/* Configures the maximum number of nested arrays/objects allowed when
 * encoding */
static int json_cfg_decode_max_depth(lua_State *l)
{
    json_config_t *cfg = json_arg_init(l, 1);

    return json_integer_option(l, 1, &cfg->decode_max_depth, 1, INT_MAX);
}

/* Configures number precision when converting doubles to text */
static int json_cfg_encode_number_precision(lua_State *l)
{
    json_config_t *cfg = json_arg_init(l, 1);
    int r = json_integer_option(l, 1, &cfg->encode_number_precision, 1, 16);
    return r;
}

/* Configures JSON encoding converting utf-8 text to \uxxxx */
/* false, true, char */
static int json_cfg_encode_escape_utf8(lua_State* l)
{
    json_config_t* cfg = json_arg_init(l, 1);
    const unsigned char* str;
    size_t len;

    switch (lua_type(l, 1)) {
    case LUA_TBOOLEAN:
        cfg->encode_escape_utf8 = lua_toboolean(l, 1) ? 1 : 0;
        break;
    case LUA_TSTRING:
        str = (unsigned char*)lua_tolstring(l, 1, &len);
        if (len > 0) {
            cfg->encode_escape_utf8 = -1 * (unsigned int)str[0];
        }
        break;
    }

    return 1;
}

/* Configures how to treat empty table when encode lua table */
static int json_cfg_encode_empty_table_as_object(lua_State *l)
{
    json_config_t *cfg = json_arg_init(l, 1);

    return json_enum_option(l, 1, &cfg->encode_empty_table_as_object, NULL, 1);
}

/* Configures JSON encoding buffer persistence */
static int json_cfg_encode_keep_buffer(lua_State *l)
{
    json_config_t *cfg = json_arg_init(l, 1);
    int old_value;

    old_value = cfg->encode_keep_buffer;

    json_enum_option(l, 1, &cfg->encode_keep_buffer, NULL, 1);

    /* Init / free the buffer if the setting has changed */
    if (old_value ^ cfg->encode_keep_buffer) {
        if (cfg->encode_keep_buffer)
            strbuf_init(&cfg->encode_buf, 0);
        else
            strbuf_free(&cfg->encode_buf);
    }

    return 1;
}

#if defined(DISABLE_INVALID_NUMBERS) && !defined(USE_INTERNAL_FPCONV)
void json_verify_invalid_number_setting(lua_State *l, int *setting)
{
    if (*setting == 1) {
        *setting = 0;
        luaL_error(l, "Infinity, NaN, and/or hexadecimal numbers are not supported.");
    }
}
#else
#define json_verify_invalid_number_setting(l, s)    do { } while(0)
#endif

static int json_cfg_encode_invalid_numbers(lua_State *l)
{
    static const char *options[] = { "off", "on", "null", NULL };
    json_config_t *cfg = json_arg_init(l, 1);

    json_enum_option(l, 1, &cfg->encode_invalid_numbers, options, 1);

    json_verify_invalid_number_setting(l, &cfg->encode_invalid_numbers);

    return 1;
}

static int json_cfg_decode_invalid_numbers(lua_State *l)
{
    json_config_t *cfg = json_arg_init(l, 1);

    json_enum_option(l, 1, &cfg->decode_invalid_numbers, NULL, 1);

    json_verify_invalid_number_setting(l, &cfg->encode_invalid_numbers);

    return 1;
}

static void json_push_registry_table(lua_State* l)
{
    lua_pushlightuserdata(l, CJSON_LIGHTUSERDATA);
    lua_rawget(l, LUA_REGISTRYINDEX);
}

static int json_cfg_set_invalid_type_encoder(lua_State* l)
{
    luaL_checktype(l, 1, LUA_TFUNCTION); // [func]
    lua_pushnumber(l, CJSON_REGISTRY_INVALID_TYPE_ENCODER); // [func, idx]
    json_push_registry_table(l); // [func, idx, cjson_registry]
    lua_insert(l, -3); // [cjson_registry, func, idx]
    lua_insert(l, -2); // [cjson_registry, idx, func]
    lua_rawset(l, -3); // [cjson_registry]
    lua_pop(l, 1);
    return 0;
}

static int json_push_invalid_type_encoder(lua_State* l)
{
    json_push_registry_table(l); // [cjson_registry]
    lua_pushnumber(l, CJSON_REGISTRY_INVALID_TYPE_ENCODER); // [cjson_registry, idx]
    lua_rawget(l, -2); // [cjson_registry, invalid_type_encoder]
    lua_remove(l, -2); // [invalid_type_encoder]
    if (lua_type(l, -1) == LUA_TNIL) {
        lua_pop(l, 1);
        return 0;
    }
    else {
        return 1;
    }
}

static int json_cfg_encode_escape_forward_slash(lua_State *l)
{
    int            ret;
    json_config_t *cfg = json_arg_init(l, 1);

    ret = json_enum_option(l, 1, &cfg->encode_escape_forward_slash, NULL, 1);
    if (cfg->encode_escape_forward_slash) {
        char2escape['/'] = "\\/";
    } else {
        char2escape['/'] = NULL;
    }
    return ret;
}

static int json_cfg_encode_numbers_as_base64(lua_State* l)
{
    json_config_t* cfg = json_arg_init(l, 1);
    int ret = json_enum_option(l, 1, &cfg->encode_numbers_as_base64, NULL, 1);
    return ret;
}


static int json_cfg_decode_numbers_as_base64(lua_State* l)
{
    json_config_t* cfg = json_arg_init(l, 1);
    int ret = json_enum_option(l, 1, &cfg->decode_numbers_as_base64, NULL, 1);
    return ret;
}

static int json_destroy_config(lua_State *l)
{
    json_config_t *cfg;

    cfg = lua_touserdata(l, 1);
    if (cfg)
        strbuf_free(&cfg->encode_buf);
    cfg = NULL;

    return 0;
}

static void json_create_config(lua_State *l)
{
    json_config_t *cfg;
    int i;

    cfg = lua_newuserdata(l, sizeof(*cfg));

    /* Create GC method to clean up strbuf */
    lua_newtable(l);
    lua_pushcfunction(l, json_destroy_config);
    lua_setfield(l, -2, "__gc");
    lua_setmetatable(l, -2);

    cfg->encode_sparse_convert = DEFAULT_SPARSE_CONVERT;
    cfg->encode_sparse_ratio = DEFAULT_SPARSE_RATIO;
    cfg->encode_sparse_safe = DEFAULT_SPARSE_SAFE;
    cfg->encode_max_depth = DEFAULT_ENCODE_MAX_DEPTH;
    cfg->decode_max_depth = DEFAULT_DECODE_MAX_DEPTH;
    cfg->encode_invalid_numbers = DEFAULT_ENCODE_INVALID_NUMBERS;
    cfg->decode_invalid_numbers = DEFAULT_DECODE_INVALID_NUMBERS;
    cfg->encode_keep_buffer = DEFAULT_ENCODE_KEEP_BUFFER;
    cfg->encode_number_precision = DEFAULT_ENCODE_NUMBER_PRECISION;
    cfg->encode_empty_table_as_object = DEFAULT_ENCODE_EMPTY_TABLE_AS_OBJECT;
    cfg->encode_escape_forward_slash = DEFAULT_ENCODE_ESCAPE_FORWARD_SLASH;
    cfg->encode_escape_utf8 = DEFAULT_ENCODE_ESCAPE_UTF8;
    cfg->encode_numbers_as_base64 = DEFAULT_ENCODE_NUMBERS_AS_BASE64;
    cfg->decode_numbers_as_base64 = DEFAULT_ENCODE_NUMBERS_AS_BASE64;

#if DEFAULT_ENCODE_KEEP_BUFFER > 0
    strbuf_init(&cfg->encode_buf, 0);
#endif

    /* Decoding init */

    /* Tag all characters as an error */
    for (i = 0; i < 256; i++)
        cfg->ch2token[i] = T_ERROR;

    /* Set tokens that require no further processing */
    cfg->ch2token['{'] = T_OBJ_BEGIN;
    cfg->ch2token['}'] = T_OBJ_END;
    cfg->ch2token['['] = T_ARR_BEGIN;
    cfg->ch2token[']'] = T_ARR_END;
    cfg->ch2token[','] = T_COMMA;
    cfg->ch2token[':'] = T_COLON;
    cfg->ch2token['\0'] = T_END;
    cfg->ch2token[' '] = T_WHITESPACE;
    cfg->ch2token['\t'] = T_WHITESPACE;
    cfg->ch2token['\n'] = T_WHITESPACE;
    cfg->ch2token['\r'] = T_WHITESPACE;

    /* Update characters that require further processing */
    cfg->ch2token['f'] = T_UNKNOWN;     /* false? */
    cfg->ch2token['i'] = T_UNKNOWN;     /* inf, ininity? */
    cfg->ch2token['I'] = T_UNKNOWN;
    cfg->ch2token['n'] = T_UNKNOWN;     /* null, nan? */
    cfg->ch2token['N'] = T_UNKNOWN;
    cfg->ch2token['t'] = T_UNKNOWN;     /* true? */
    cfg->ch2token['"'] = T_UNKNOWN;     /* string? */
    cfg->ch2token['+'] = T_UNKNOWN;     /* number? */
    cfg->ch2token['-'] = T_UNKNOWN;
    for (i = 0; i < 10; i++)
        cfg->ch2token['0' + i] = T_UNKNOWN;

    /* Lookup table for parsing escape characters */
    for (i = 0; i < 256; i++)
        cfg->escape2char[i] = 0;          /* String error */
    cfg->escape2char['"'] = '"';
    cfg->escape2char['\\'] = '\\';
    cfg->escape2char['/'] = '/';
    cfg->escape2char['b'] = '\b';
    cfg->escape2char['t'] = '\t';
    cfg->escape2char['n'] = '\n';
    cfg->escape2char['f'] = '\f';
    cfg->escape2char['r'] = '\r';
    cfg->escape2char['u'] = 'u';          /* Unicode parsing required */
}

/* ===== ENCODING ===== */

static void json_encode_exception(lua_State *l, json_config_t *cfg, strbuf_t *json, int lindex,
                                  const char *reason)
{
    if (!cfg->encode_keep_buffer)
        strbuf_free(json);
    luaL_error(l, "Cannot serialise %s: %s",
                  lua_typename(l, lua_type(l, lindex)), reason);
}

/* json_append_string args:
 * - lua_State
 * - JSON config
 * - JSON strbuf
 * - String (Lua stack index)
 *
 * Returns nothing. Doesn't remove string from Lua stack */
static void json_append_string(lua_State* l, json_config_t* cfg, strbuf_t* json, int lindex)
{
    const char *escstr;
    unsigned i;
    const char *str;
    size_t len;

    /* for utf8 escape */
    uint32_t prev_state = 0;
    uint32_t state = 0;
    uint32_t codepoint;
    unsigned char utf8_place_char;
    char utf8_buf[7];

    str = lua_tolstring(l, lindex, &len);

    /* Worst case is len * 6 (all unicode escapes).
     * This buffer is reused constantly for small strings
     * If there are any excess pages, they won't be hit anyway.
     * This gains ~5% speedup. */
    size_t buf_len = len * 6 + 2;
    if (buf_len > INT_MAX) buf_len = INT_MAX;
    strbuf_ensure_empty_length(json, (int)buf_len);

    strbuf_append_char_unsafe(json, '\"');
    if (cfg->encode_escape_utf8 < 0 || cfg->encode_escape_utf8 == 1) {
        utf8_place_char = cfg->encode_escape_utf8 == 1 ? 0 : ((unsigned char)(-1 * cfg->encode_escape_utf8));
        /* fprintf(stderr, "%c", utf8_place_char); */

        for (i = 0; i < len; prev_state = state, ++i) {
            switch (utf8_decode(&state, &codepoint, (unsigned char)str[i])) {
            case UTF8_ACCEPT:
                /* A properly encoded character has been found. */
                if (codepoint >= 0 && codepoint <= 127) {
                    escstr = char2escape[(unsigned char)codepoint];
                    if (escstr)
                        strbuf_append_string(json, escstr);
                    else
                        strbuf_append_char_unsafe(json, codepoint);
                }
                else {
                    snprintf(utf8_buf, 7, "\\u%04x", codepoint);
                    strbuf_append_string(json, utf8_buf);
                }
                break;

            case UTF8_REJECT:
                /* The byte is invalid, replace it and restart. */
                if (!utf8_place_char) {
                    luaL_error(l, "utf8 string (%s) invalid at (%d)", str, i);
                }

                strbuf_append_char_unsafe(json, utf8_place_char);
                state = UTF8_ACCEPT;
                if (prev_state != UTF8_ACCEPT)
                    --i;
                break;
            }
        }
    }
    else {
        for (i = 0; i < len; i++) {
            escstr = char2escape[(unsigned char)str[i]];
            if (escstr)
                strbuf_append_string(json, escstr);
            else
                strbuf_append_char_unsafe(json, str[i]);
        }
    }

    strbuf_append_char_unsafe(json, '\"');
}

/* Find the size of the array on the top of the Lua stack
 * -1   object (not a pure array)
 * >=0  elements in array
 */
static int lua_array_length(lua_State *l, json_config_t *cfg, strbuf_t *json)
{
    double k;
    size_t max;
    size_t items;

    max = 0;
    items = 0;

    if (luaL_getmetafield(l, -1, "__pairs")) {
        int index = lua_gettop(l) - 1;
        int type;
        lua_pushvalue(l, -2);
        lua_call(l, 1, 3);
        lua_newtable(l);
        lua_replace(l, index);
        while (1) {
            lua_pushvalue(l, -2);
            lua_pushvalue(l, -2);
            lua_copy(l, -5, -3);
            lua_call(l, 2, 2);
            type = lua_type(l, -2);
            if (type == LUA_TNIL)
                break;
            if (items >= 0) {
                if (type == LUA_TNUMBER &&
                    (k = lua_tonumber(l, -2))) {
                    if (floor(k) == k && k >= -1) {
                        if (k > max)
                            max = (size_t)k;
                        items++;
                    }
                    else {
                        items = -1;
                    }
                }
                else {
                    items = -1;
                }
            }
            lua_pushvalue(l, -1);
            lua_copy(l, -3, -2);
            lua_rawset(l, index);
        }
        lua_settop(l, index);
        if (items < 0)
            return -1;
    }
    else {
        lua_pushnil(l);
        /* table, startkey */
        while (lua_next(l, -2) != 0) {
            /* table, key, value */
            if (lua_type(l, -2) == LUA_TNUMBER &&
                (k = lua_tonumber(l, -2))) {
                /* Integer >= 1 ? */
                if (floor(k) == k && k >= 1) {
                    if (k > max)
                        max = (size_t)k;
                    items++;
                    lua_pop(l, 1);
                    continue;
                }
            }
            /* Must not be an array (non integer key) */
            lua_pop(l, 2);
            return -1;
        }
    }

    /* Encode excessively sparse arrays as objects (if enabled) */
    if (cfg->encode_sparse_ratio > 0 &&
        max > items * cfg->encode_sparse_ratio &&
        max > cfg->encode_sparse_safe) {
        if (!cfg->encode_sparse_convert)
            json_encode_exception(l, cfg, json, -1, "excessively sparse array");

        return -1;
    }

    if (max > INT_MAX) max = INT_MAX;
    return (int)max;
}

static void json_check_encode_depth(lua_State *l, json_config_t *cfg,
                                    int current_depth, strbuf_t *json)
{
    /* Ensure there are enough slots free to traverse a table (key,
     * value) and push a string for a potential error message.
     *
     * Unlike "decode", the key and value are still on the stack when
     * lua_checkstack() is called.  Hence an extra slot for luaL_error()
     * below is required just in case the next check to lua_checkstack()
     * fails.
     *
     * While this won't cause a crash due to the EXTRA_STACK reserve
     * slots, it would still be an improper use of the API. */
    if (current_depth <= cfg->encode_max_depth && lua_checkstack(l, 3))
        return;

    if (!cfg->encode_keep_buffer)
        strbuf_free(json);

    luaL_error(l, "Cannot serialise, excessive nesting (%d)",
               current_depth);
}

static void json_append_data(lua_State *l, json_config_t *cfg,
                             int current_depth, strbuf_t *json);

/* json_append_array args:
 * - lua_State
 * - JSON strbuf
 * - Size of passwd Lua array (top of stack) */
static void json_append_array(lua_State *l, json_config_t *cfg, int current_depth,
                              strbuf_t *json, int array_length)
{
    int comma, i;

    strbuf_append_char(json, '[');

    comma = 0;
    for (i = 1; i <= array_length; i++) {
        if (comma)
            strbuf_append_char(json, ',');
        else
            comma = 1;

        lua_rawgeti(l, -1, i);
        json_append_data(l, cfg, current_depth, json);
        lua_pop(l, 1);
    }

    strbuf_append_char(json, ']');
}

// base64 encoded value should be at least: 17 chars = 12chars + header 2 chars + 2x'"' + "\0"
// "L=0123456789AB\0" or "D=0123456789AB\0"
// header should be with heading "
static int json_append_number_as_base64(strbuf_t* json, bytemap64 nb, const char* header, const int header_len)
{
    //int buf_len = base64_req_len(sizeof(nb));
    const int buf_len = 13;
    strbuf_ensure_empty_length(json, buf_len + header_len + 1);
    strbuf_append_mem_unsafe(json, header, header_len);
    int len = int64_to_base64(nb, strbuf_empty_ptr(json), buf_len);
    if (len > 0)
        strbuf_extend_length(json, len);
    strbuf_append_char(json, '"');
    return (len > 0);
}

static void json_append_number(lua_State* l, json_config_t* cfg,
    strbuf_t* json, int lindex)
{
    int len = 0;

#if LUA_VERSION_NUM >= 503
    if (lua_isinteger(l, lindex)) {
        lua_Integer num = lua_tointeger(l, lindex);
        strbuf_ensure_empty_length(json, ITOA_BUFSIZE); /* max length of int64 is 22 */
        len = itoa_vitaut(num, strbuf_empty_ptr(json), ITOA_BUFSIZE);
        if (len > 0)
            strbuf_extend_length(json, len);
        else
            json_encode_exception(l, cfg, json, lindex, "itoa fail!");
        return;
    }
#endif

    if (cfg->encode_numbers_as_base64)
    {
        //int buf_len = base64_req_len(sizeof(nb));
        // base64 encoded value should be at least: 17 chars = 12chars + header 2 chars + 2x'"' + "\0"
        // "L=0123456789AB\0" or "D=0123456789AB\0"
        bytemap64 nb;
        nb.as_double = lua_tonumber(l, lindex);
        const char header[] = "\"D=";
        if (!json_append_number_as_base64(json, nb, header, sizeof(header) - 1))
            json_encode_exception(l, cfg, json, lindex, "json_append_number_as_base64 fail!");
        return;
    }

    double num = lua_tonumber(l, lindex);

    if (cfg->encode_invalid_numbers == 0) {
        /* Prevent encoding invalid numbers */
        if (isinf(num) || isnan(num))
            json_encode_exception(l, cfg, json, lindex,
                "must not be NaN or Infinity");
    }
    else if (cfg->encode_invalid_numbers == 1) {
        /* Encode NaN/Infinity separately to ensure Javascript compatible
         * values are used. */
        if (isnan(num)) {
            strbuf_append_mem(json, "NaN", 3);
            return;
        }
        if (isinf(num)) {
            if (num < 0)
                strbuf_append_mem(json, "-Infinity", 9);
            else
                strbuf_append_mem(json, "Infinity", 8);
            return;
        }
    }
    else {
        /* Encode invalid numbers as "null" */
        if (isinf(num) || isnan(num)) {
            strbuf_append_mem(json, "null", 4);
            return;
        }
    }

    strbuf_ensure_empty_length(json, _CVTBUFSIZE);
    if (0 == _gcvt_s(strbuf_empty_ptr(json), _CVTBUFSIZE, num, cfg->encode_number_precision))
    {
        len = (int)strnlen_s(strbuf_empty_ptr(json), _CVTBUFSIZE);
        if (len > 0)
        {
            strbuf_extend_length(json, len);
            return;
        }
    }

    json_encode_exception(l, cfg, json, lindex, "Double to String conversion Fail!");
}

static int json_append_tojson_invoke(lua_State* l, json_config_t* cfg, strbuf_t* json) {
    // [data]
    if (lua_getmetatable(l, -1)) { // [data, mt]
        lua_getfield(l, -1, "__tojson"); // [data, mt, __tojson]
        if (lua_type(l, -1) == LUA_TFUNCTION) {
            lua_pushvalue(l, -3); // [data, mt, __tojson, data]
            lua_call(l, 1, 1); // [data, mt, string]
            if (lua_type(l, -1) != LUA_TSTRING) {
                json_encode_exception(l, cfg, json, -1, "__tojson(v) did not return a string");
                /* never returns */
            }
            strbuf_append_string(json, lua_tolstring(l, -1, 0));
            lua_settop(l, -3);
            return 1;
        }
        lua_settop(l, -3);
    }
    return 0;
}


static void json_append_object(lua_State *l, json_config_t *cfg,
                               int current_depth, strbuf_t *json)
{
    int comma, keytype;

    /* Object */
    strbuf_append_char(json, '{');

    lua_pushnil(l);
    /* table, startkey */
    comma = 0;
    while (lua_next(l, -2) != 0) {
        if (comma)
            strbuf_append_char(json, ',');
        else
            comma = 1;

        /* table, key, value */
        keytype = lua_type(l, -2);
        if (keytype == LUA_TNUMBER) {
            strbuf_append_char(json, '"');
            json_append_number(l, cfg, json, -2);
            strbuf_append_mem(json, "\":", 2);
        } else if (keytype == LUA_TSTRING) {
            json_append_string(l, cfg, json, -2);
            strbuf_append_char(json, ':');
        } else {
            json_encode_exception(l, cfg, json, -2,
                                  "table key must be a number or string");
            /* never returns */
        }

        /* table, key, value */
        json_append_data(l, cfg, current_depth, json);
        lua_pop(l, 1);
        /* table, key */
    }

    strbuf_append_char(json, '}');
}

/* Serialise Lua data into JSON string. */
static void json_append_data(lua_State *l, json_config_t *cfg,
                             int current_depth, strbuf_t *json)
{
    int len;
    switch (lua_type(l, -1)) {
    case LUA_TSTRING:
        json_append_string(l, cfg, json, -1);
        break;
    case LUA_TNUMBER:
        json_append_number(l, cfg, json, -1);
        break;
    case LUA_TBOOLEAN:
        if (lua_toboolean(l, -1))
            strbuf_append_mem(json, "true", 4);
        else
            strbuf_append_mem(json, "false", 5);
        break;
    case LUA_TTABLE:
        // invoke getmetatable(val).__tojson() if present #14
        // https://github.com/mpx/lua-cjson/pull/14/commits/639d93954c38bbca28ab07b8786fe488bb8d4f3c
        if (json_append_tojson_invoke(l, cfg, json)) { break; }

        current_depth++;
        json_check_encode_depth(l, cfg, current_depth, json);

        len = lua_array_length(l, cfg, json);

        if (len > 0 || (len == 0 && !cfg->encode_empty_table_as_object)) {
            json_append_array(l, cfg, current_depth, json, len);
        } else {
            json_append_object(l, cfg, current_depth, json);
        }
        break;
    case LUA_TNIL:
        strbuf_append_mem(json, "null", 4);
        break;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(l, -1) == NULL) {
            strbuf_append_mem(json, "null", 4);
        }
        break;
    default:
        /* Remaining types (LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD,
         * and LUA_TLIGHTUSERDATA) cannot be serialised */
        if (json_push_invalid_type_encoder(l)) { // [data, invalid_type_encoder]
            lua_pushvalue(l, -2); // [data, invalid_type_encoder, data]
            lua_call(l, 1, 1); // [data, encoder_result]
            if (lua_type(l, -1) != LUA_TSTRING) {
                json_encode_exception(l, cfg, json, -1, "invalid_type_encoder(v) did not return a string");
                /* never returns */
            }
            else {
                strbuf_append_string(json, lua_tolstring(l, -1, 0));
                lua_pop(l, 1); // [data]
            }
        }
        else {
            json_encode_exception(l, cfg, json, -1, "type not supported");
            /* never returns */
        }
    }
}

static int json_encode(lua_State *l)
{
    json_config_t *cfg = json_fetch_config(l);
    strbuf_t local_encode_buf;
    strbuf_t *encode_buf;
    char *json;
    int len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    if (!cfg->encode_keep_buffer) {
        /* Use private buffer */
        encode_buf = &local_encode_buf;
        strbuf_init(encode_buf, 0);
    } else {
        /* Reuse existing buffer */
        encode_buf = &cfg->encode_buf;
        strbuf_reset(encode_buf);
    }

    json_append_data(l, cfg, 0, encode_buf);
    json = strbuf_string(encode_buf, &len);

    lua_pushlstring(l, json, len);

    if (!cfg->encode_keep_buffer)
        strbuf_free(encode_buf);

    return 1;
}

/* ===== DECODING ===== */

static void json_process_value(lua_State *l, json_parse_t *json,
                               json_token_t *token);

static int hexdigit2int(char hex)
{
    if ('0' <= hex  && hex <= '9')
        return hex - '0';

    /* Force lowercase */
    hex |= 0x20;
    if ('a' <= hex && hex <= 'f')
        return 10 + hex - 'a';

    return -1;
}

static int decode_hex4(const char *hex)
{
    int digit[4];
    int i;

    /* Convert ASCII hex digit to numeric digit
     * Note: this returns an error for invalid hex digits, including
     *       NULL */
    for (i = 0; i < 4; i++) {
        digit[i] = hexdigit2int(hex[i]);
        if (digit[i] < 0) {
            return -1;
        }
    }

    return (digit[0] << 12) +
           (digit[1] << 8) +
           (digit[2] << 4) +
            digit[3];
}

/* Converts a Unicode codepoint to UTF-8.
 * Returns UTF-8 string length, and up to 4 bytes in *utf8 */
static int codepoint_to_utf8(char *utf8, int codepoint)
{
    /* 0xxxxxxx */
    if (codepoint <= 0x7F) {
        utf8[0] = codepoint;
        return 1;
    }

    /* 110xxxxx 10xxxxxx */
    if (codepoint <= 0x7FF) {
        utf8[0] = (codepoint >> 6) | 0xC0;
        utf8[1] = (codepoint & 0x3F) | 0x80;
        return 2;
    }

    /* 1110xxxx 10xxxxxx 10xxxxxx */
    if (codepoint <= 0xFFFF) {
        utf8[0] = (codepoint >> 12) | 0xE0;
        utf8[1] = ((codepoint >> 6) & 0x3F) | 0x80;
        utf8[2] = (codepoint & 0x3F) | 0x80;
        return 3;
    }

    /* 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if (codepoint <= 0x1FFFFF) {
        utf8[0] = (codepoint >> 18) | 0xF0;
        utf8[1] = ((codepoint >> 12) & 0x3F) | 0x80;
        utf8[2] = ((codepoint >> 6) & 0x3F) | 0x80;
        utf8[3] = (codepoint & 0x3F) | 0x80;
        return 4;
    }

    return 0;
}


/* Called when index pointing to beginning of UTF-16 code escape: \uXXXX
 * \u is guaranteed to exist, but the remaining hex characters may be
 * missing.
 * Translate to UTF-8 and append to temporary token string.
 * Must advance index to the next character to be processed.
 * Returns: 0   success
 *          -1  error
 */
static int json_append_unicode_escape(json_parse_t *json)
{
    char utf8[4];       /* Surrogate pairs require 4 UTF-8 bytes */
    int codepoint;
    int surrogate_low;
    int len;
    int escape_len = 6;

    /* Fetch UTF-16 code unit */
    codepoint = decode_hex4(json->ptr + 2);
    if (codepoint < 0)
        return -1;

    /* UTF-16 surrogate pairs take the following 2 byte form:
     *      11011 x yyyyyyyyyy
     * When x = 0: y is the high 10 bits of the codepoint
     *      x = 1: y is the low 10 bits of the codepoint
     *
     * Check for a surrogate pair (high or low) */
    if ((codepoint & 0xF800) == 0xD800) {
        /* Error if the 1st surrogate is not high */
        if (codepoint & 0x400)
            return -1;

        /* Ensure the next code is a unicode escape */
        if (*(json->ptr + escape_len) != '\\' ||
            *(json->ptr + escape_len + 1) != 'u') {
            return -1;
        }

        /* Fetch the next codepoint */
        surrogate_low = decode_hex4(json->ptr + 2 + escape_len);
        if (surrogate_low < 0)
            return -1;

        /* Error if the 2nd code is not a low surrogate */
        if ((surrogate_low & 0xFC00) != 0xDC00)
            return -1;

        /* Calculate Unicode codepoint */
        codepoint = (codepoint & 0x3FF) << 10;
        surrogate_low &= 0x3FF;
        codepoint = (codepoint | surrogate_low) + 0x10000;
        escape_len = 12;
    }

    /* Convert codepoint to UTF-8 */
    len = codepoint_to_utf8(utf8, codepoint);
    if (!len)
        return -1;

    /* Append bytes and advance parse index */
    strbuf_append_mem_unsafe(json->tmp, utf8, len);
    json->ptr += escape_len;

    return 0;
}

static void json_set_token_error(json_token_t *token, json_parse_t *json,
                                 const char *errtype)
{
    token->type = T_ERROR;
    size_t idx = json->ptr - json->data;
    if (idx > INT_MAX) idx = INT_MAX;
    token->index = (int)idx;
    token->value.string = errtype;
}

static void json_next_string_token(json_parse_t *json, json_token_t *token)
{
    char *escape2char = json->cfg->escape2char;
    char ch;

    /* Caller must ensure a string is next */
    assert(*json->ptr == '"');

    /* Skip " */
    json->ptr++;

    /* json->tmp is the temporary strbuf used to accumulate the
     * decoded string value.
     * json->tmp is sized to handle JSON containing only a string value.
     */
    strbuf_reset(json->tmp);

    while ((ch = *json->ptr) != '"') {
        if (!ch) {
            /* Premature end of the string */
            json_set_token_error(token, json, "unexpected end of string");
            return;
        }

        /* Handle escapes */
        if (ch == '\\') {
            /* Fetch escape character */
            ch = *(json->ptr + 1);

            /* Translate escape code and append to tmp string */
            ch = escape2char[(unsigned char)ch];
            if (ch == 'u') {
                if (json_append_unicode_escape(json) == 0)
                    continue;

                json_set_token_error(token, json,
                                     "invalid unicode escape code");
                return;
            }
            if (!ch) {
                json_set_token_error(token, json, "invalid escape code");
                return;
            }

            /* Skip '\' */
            json->ptr++;
        }
        /* Append normal character or translated single character
         * Unicode escapes are handled above */
        strbuf_append_char_unsafe(json->tmp, ch);
        json->ptr++;
    }
    json->ptr++;    /* Eat final quote (") */

    strbuf_ensure_null(json->tmp);

    token->type = T_STRING;
    token->value.string = strbuf_string(json->tmp, &token->string_len);
}

// Try to decode base64 encoded INT64 or Decimal (64bit), if success returns true
static int json_next_base64_number_token(json_parse_t* json, json_token_t* token)
{
    const char *head_lng = "\"L=";
    const char *head_dbl = "\"D=";
    int is_long = strncmp(json->ptr, head_lng, 3) == 0;
    int is_double = strncmp(json->ptr, head_dbl, 3) == 0;

    if (!is_long && !is_double)
        return 0;

    // calc token len
    const char* b64ptr = json->ptr + 3;
    const char* endptr = b64ptr;
    while (*endptr != '"')
    {
        if (!*endptr)
        {
            /* Premature end of the string */
            json_set_token_error(token, json, "unexpected end of string");
            return 0;
        }
        endptr++;
    }

    // base64 encoded value should be at least 12chars
    // L=0123456789AB
    if (endptr - b64ptr < 12)
        return 0;

    bytemap64 b64;
    if (!base64_to_bin64(b64ptr, &b64))
        return 0;  // Invalid base64 encoded number

    if (is_double) {
        token->type = T_NUMBER;
        token->value.number = b64.as_double;
    }
    else
    {
        token->type = T_INTEGER;
        token->value.integer = b64.as_int64;
    }
    // Skip the processed number, Eat final quote (")
    json->ptr = endptr + 1;
    return 1;
}

/* Fills in the token struct.
 * T_STRING will return a pointer to the json_parse_t temporary string
 * T_ERROR will leave the json->ptr pointer at the error.
 */
static void json_next_token(json_parse_t *json, json_token_t *token)
{
    const json_token_type_t *ch2token = json->cfg->ch2token;
    int ch;

    /* Eat whitespace. */
    while (1) {
        ch = (unsigned char)*(json->ptr);
        token->type = ch2token[ch];
        if (token->type != T_WHITESPACE)
            break;
        json->ptr++;
    }

    /* Store location of new token. Required when throwing errors
     * for unexpected tokens (syntax errors). */
    size_t idx = json->ptr - json->data;
    if (idx > INT_MAX) idx = INT_MAX;
    token->index = (int)idx;

    /* Don't advance the pointer for an error or the end */
    if (token->type == T_ERROR) {
        json_set_token_error(token, json, "invalid token");
        return;
    }

    if (token->type == T_END) {
        return;
    }

    /* Found a known single character token, advance index and return */
    if (token->type != T_UNKNOWN) {
        json->ptr++;
        return;
    }

    /* Process characters which triggered T_UNKNOWN
     *
     * Must use strncmp() to match the front of the JSON string.
     * JSON identifier must be lowercase.
     * When strict_numbers if disabled, either case is allowed for
     * Infinity/NaN (since we are no longer following the spec..) */
    if (ch == '"') 
    {
        // Try to decode base64 encoded INT64 or Decimal (64bit), if success returns true
        if (json->cfg->decode_numbers_as_base64 && json_next_base64_number_token(json, token))
            return;
        json_next_string_token(json, token);
        return;
    }
    else if (!strncmp(json->ptr, "true", 4)) {
        token->type = T_BOOLEAN;
        token->value.boolean = 1;
        json->ptr += 4;
        return;
    }
    else if (!strncmp(json->ptr, "false", 5)) {
        token->type = T_BOOLEAN;
        token->value.boolean = 0;
        json->ptr += 5;
        return;
    }
    else if (!strncmp(json->ptr, "null", 4)) {
        token->type = T_NULL;
        json->ptr += 4;
        return;
    }
    else if (json_next_number_token(json, token))
        return;

    /* Token starts with t/f/n but isn't recognised above. */
    json_set_token_error(token, json, "invalid token");
}

/* This function does not return.
 * DO NOT CALL WITH DYNAMIC MEMORY ALLOCATED.
 * The only supported exception is the temporary parser string
 * json->tmp struct.
 * json and token should exist on the stack somewhere.
 * luaL_error() will long_jmp and release the stack */
static void json_throw_parse_error(lua_State *l, json_parse_t *json,
                                   const char *exp, json_token_t *token)
{
    const char *found;

    strbuf_free(json->tmp);

    if (token->type == T_ERROR)
        found = token->value.string;
    else
        found = json_token_type_name[token->type];

    /* Note: token->index is 0 based, display starting from 1 */
    luaL_error(l, "Expected %s but found %s at character %d",
               exp, found, token->index + 1);
}

static inline void json_decode_ascend(json_parse_t *json)
{
    json->current_depth--;
}

static void json_decode_descend(lua_State *l, json_parse_t *json, int slots)
{
    json->current_depth++;

    if (json->current_depth <= json->cfg->decode_max_depth &&
        lua_checkstack(l, slots)) {
        return;
    }

    strbuf_free(json->tmp);
    luaL_error(l, "Found too many nested data structures (%d) at character %d",
        json->current_depth, json->ptr - json->data);
}

static void json_set_metatable_field_string(lua_State* l, char* k, char* v)
{
    // [table]
    if (!lua_getmetatable(l, -1)) {
        lua_newtable(l); // [table, mt]
        lua_pushvalue(l, -1); // [table, mt, mt]
        lua_setmetatable(l, -3); // [table, mt]
    }
    // [table, mt]
    lua_pushstring(l, v); // [table, mt, v]
    lua_setfield(l, -2, k); // [table, mt]
    lua_pop(l, 1); // [table]
}

static void json_parse_object_context(lua_State *l, json_parse_t *json)
{
    json_token_t token;

    /* 3 slots required:
     * .., table, key, value */
    json_decode_descend(l, json, 3);

    lua_newtable(l);
    json_set_metatable_field_string(l, "__jsontype", "object");

    json_next_token(json, &token);

    /* Handle empty objects */
    if (token.type == T_OBJ_END) {
        json_decode_ascend(json);
        return;
    }

    while (1) {
        if (token.type != T_STRING)
            json_throw_parse_error(l, json, "object key string", &token);

        /* Push key */
        lua_pushlstring(l, token.value.string, token.string_len);

        json_next_token(json, &token);
        if (token.type != T_COLON)
            json_throw_parse_error(l, json, "colon", &token);

        /* Fetch value */
        json_next_token(json, &token);
        json_process_value(l, json, &token);

        /* Set key = value */
        lua_rawset(l, -3);

        json_next_token(json, &token);

        if (token.type == T_OBJ_END) {
            json_decode_ascend(json);
            return;
        }

        if (token.type != T_COMMA)
            json_throw_parse_error(l, json, "comma or object end", &token);

        json_next_token(json, &token);
    }
}

/* Handle the array context */
static void json_parse_array_context(lua_State *l, json_parse_t *json)
{
    json_token_t token;
    int i;

    /* 2 slots required:
     * .., table, value */
    json_decode_descend(l, json, 2);

    lua_newtable(l);
    json_set_metatable_field_string(l, "__jsontype", "array");

    json_next_token(json, &token);

    /* Handle empty arrays */
    if (token.type == T_ARR_END) {
        json_decode_ascend(json);
        return;
    }

    for (i = 1; ; i++) {
        json_process_value(l, json, &token);
        lua_rawseti(l, -2, i);            /* arr[i] = value */

        json_next_token(json, &token);

        if (token.type == T_ARR_END) {
            json_decode_ascend(json);
            return;
        }

        if (token.type != T_COMMA)
            json_throw_parse_error(l, json, "comma or array end", &token);

        json_next_token(json, &token);
    }
}

/* Handle the "value" context */
static void json_process_value(lua_State *l, json_parse_t *json,
                               json_token_t *token)
{
    switch (token->type) {
    case T_STRING:
        lua_pushlstring(l, token->value.string, token->string_len);
        break;;
    case T_NUMBER:
        lua_pushnumber(l, token->value.number);
        break;;
    case T_INTEGER:
        lua_pushinteger(l, token->value.integer);
        break;;
    case T_BOOLEAN:
        lua_pushboolean(l, token->value.boolean);
        break;;
    case T_OBJ_BEGIN:
        json_parse_object_context(l, json);
        break;;
    case T_ARR_BEGIN:
        json_parse_array_context(l, json);
        break;;
    case T_NULL:
        /* In Lua, setting "t[k] = nil" will delete k from the table.
         * Hence a NULL pointer lightuserdata object is used instead */
        lua_pushlightuserdata(l, NULL);
        break;;
    default:
        json_throw_parse_error(l, json, "value", token);
    }
}

static int json_decode(lua_State *l)
{
    json_parse_t json;
    json_token_t token;
    size_t json_len;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    json.cfg = json_fetch_config(l);
    json.data = luaL_checklstring(l, 1, &json_len);
    json.current_depth = 0;
    json.ptr = json.data;

    /* Detect Unicode other than UTF-8 (see RFC 4627, Sec 3)
     *
     * CJSON can support any simple data type, hence only the first
     * character is guaranteed to be ASCII (at worst: '"'). This is
     * still enough to detect whether the wrong encoding is in use. */
    if (json_len >= 2 && (!json.data[0] || !json.data[1]))
        luaL_error(l, "JSON parser does not support UTF-16 or UTF-32");

    /* Ensure the temporary buffer can hold the entire string.
     * This means we no longer need to do length checks since the decoded
     * string must be smaller than the entire json string */
    if (json_len > INT_MAX) json_len = INT_MAX;
    json.tmp = strbuf_new((int)json_len);

    json_next_token(&json, &token);
    json_process_value(l, &json, &token);

    /* Ensure there is no more input left */
    json_next_token(&json, &token);

    if (token.type != T_END)
        json_throw_parse_error(l, &json, "the end", &token);

    strbuf_free(json.tmp);

    return 1;
}

/* ===== INITIALISATION ===== */

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 502
/* Compatibility for Lua 5.1 and older LuaJIT.
 *
 * compat_luaL_setfuncs() is used to create a module table where the functions
 * have json_config_t as their first upvalue. Code borrowed from Lua 5.2
 * source's luaL_setfuncs().
 */
static void compat_luaL_setfuncs(lua_State *l, const luaL_Reg *reg, int nup)
{
    int i;

    luaL_checkstack(l, nup, "too many upvalues");
    for (; reg->name != NULL; reg++) {  /* fill the table with given functions */
        for (i = 0; i < nup; i++)  /* copy upvalues to the top */
            lua_pushvalue(l, -nup);
        lua_pushcclosure(l, reg->func, nup);  /* closure with those upvalues */
        lua_setfield(l, -(nup + 2), reg->name);
    }
    lua_pop(l, nup);  /* remove upvalues */
}
#else
#define compat_luaL_setfuncs(L, reg, nup) luaL_setfuncs(L, reg, nup)
#endif

/* Call target function in protected mode with all supplied args.
 * Assumes target function only returns a single non-nil value.
 * Convert and return thrown errors as: nil, "error message" */
static int json_protect_conversion(lua_State *l)
{
    int err;

    /* Deliberately throw an error for invalid arguments */
    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    /* pcall() the function stored as upvalue(1) */
    lua_pushvalue(l, lua_upvalueindex(1));
    lua_insert(l, 1);
    err = lua_pcall(l, 1, 1, 0);
    if (!err)
        return 1;

    if (err == LUA_ERRRUN) {
        lua_pushnil(l);
        lua_insert(l, -2);
        return 2;
    }

    /* Since we are not using a custom error handler, the only remaining
     * errors are memory related */
    return luaL_error(l, "Memory allocation error in CJSON protected call");
}

/* Return cjson module table */
static int lua_cjson_new(lua_State *l)
{
    luaL_Reg reg[] = {
        { "encode", json_encode },
        { "decode", json_decode },
        { "encode_empty_table_as_object", json_cfg_encode_empty_table_as_object },
        { "encode_sparse_array", json_cfg_encode_sparse_array },
        { "encode_max_depth", json_cfg_encode_max_depth },
        { "decode_max_depth", json_cfg_decode_max_depth },
        { "encode_number_precision", json_cfg_encode_number_precision },
        { "encode_keep_buffer", json_cfg_encode_keep_buffer },
        { "encode_invalid_numbers", json_cfg_encode_invalid_numbers },
        { "decode_invalid_numbers", json_cfg_decode_invalid_numbers },
        { "encode_escape_forward_slash", json_cfg_encode_escape_forward_slash },
        { "encode_escape_utf8", json_cfg_encode_escape_utf8 },
        { "set_invalid_type_encoder", json_cfg_set_invalid_type_encoder },
        { "encode_numbers_as_base64", json_cfg_encode_numbers_as_base64 },
        { "decode_numbers_as_base64", json_cfg_decode_numbers_as_base64 },
        { "new", lua_cjson_new },
        { NULL, NULL }
    };

    /* cjson module table */
    lua_newtable(l);

    /* Register functions with config data as upvalue */
    json_create_config(l);
    compat_luaL_setfuncs(l, reg, 1);

    /* Set cjson.null */
    lua_pushlightuserdata(l, NULL);
    lua_setfield(l, -2, "null");

    /* Set module name / version fields */
    lua_pushliteral(l, CJSON_MODNAME);
    lua_setfield(l, -2, "_NAME");
    lua_pushliteral(l, CJSON_VERSION);
    lua_setfield(l, -2, "_VERSION");

    /* Create our registry table */
    lua_pushlightuserdata(l, CJSON_LIGHTUSERDATA);
    lua_newtable(l);
    lua_rawset(l, LUA_REGISTRYINDEX);

    return 1;
}

/* Return cjson.safe module table */
static int lua_cjson_safe_new(lua_State *l)
{
    const char *func[] = { "decode", "encode", NULL };
    int i;

    lua_cjson_new(l);

    /* Fix new() method */
    lua_pushcfunction(l, lua_cjson_safe_new);
    lua_setfield(l, -2, "new");

    for (i = 0; func[i]; i++) {
        lua_getfield(l, -1, func[i]);
        lua_pushcclosure(l, json_protect_conversion, 1);
        lua_setfield(l, -2, func[i]);
    }

    return 1;
}

LUALIB_API  int luaopen_lua_cjson(lua_State* l)
{
    lua_cjson_new(l);

#ifdef ENABLE_CJSON_GLOBAL
    /* Register a global "cjson" table. */
    lua_pushvalue(l, -1);
    lua_setglobal(l, CJSON_MODNAME);
#endif

    /* Return cjson table */
    return 1;
}

LUALIB_API  int luaopen_lua_cjson_safe(lua_State* l)
{
    lua_cjson_safe_new(l);

    /* Return cjson.safe table */
    return 1;
}

/* vi:ai et sw=4 ts=4:
 */
