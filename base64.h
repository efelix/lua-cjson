#pragma once
/***********************************************************
* Base64 library                                           *
* Purpose: encode and decode base64 format                 *
***********************************************************/
#ifndef BASE46_H
#define BASE46_H

#include <stdlib.h>
#include <memory.h>

/***********************************************
Encodes ASCCI string into base64 format string
@param plain ASCII string to be encoded
@return encoded base64 format string
***********************************************/
// Encodes src bytes, output to dst_buf
// returns length of encoded string (positive number) or required length x -1 (negative number)
extern size_t base64_encode(void const* src, size_t size, char* dest, size_t dsize);
// mem size required for base64 encoded string. where src_len is unencoded memory block length
extern size_t base64_req_len(size_t src_len);
// Convert a base64 null-terminated string to binary format.
extern size_t base64_decode(char const* src, void* dest, size_t dsize);


typedef union {
    const char      as_bytes[];
    double          as_double;
    signed __int64  as_int64;
    lua_Integer     as_lua64;
} bytemap64;

// Convert a 8Byte binary memory block in a base64 null-terminated string.
extern int int64_to_base64(bytemap64 b64, char* dest, size_t dsize);

// Convert a base64 null-terminated string to 8Byte binary memory block in. Returns 1 if success
extern int base64_to_bin64(const char* src, bytemap64* out_b64);

#endif