/*
  Licensed under the MIT License <http://opensource.org/licenses/MIT>.
  SPDX-License-Identifier: MIT
  Permission is hereby  granted, free of charge, to any  person obtaining a copy
  of this software and associated  documentation files (the "Software"), to deal
  in the Software  without restriction, including without  limitation the rights
  to  use, copy,  modify, merge,  publish, distribute,  sublicense, and/or  sell
  copies  of  the Software,  and  to  permit persons  to  whom  the Software  is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.
  THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS OR
  IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF  MERCHANTABILITY,
  FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN NO EVENT  SHALL THE
  AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY  CLAIM,  DAMAGES OR  OTHER
  LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

*/

#include "pch.h"
#include "base64.h"

/** Lookup table that converts a integer to base64 digit. */
static char const base64encode[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Get the first base 64 digit of a block of 4.
  * @param a The first byte of the source block of 3.
  * @return A base 64 digit. */
static char get0(unsigned char a) {
    int const index = a >> 2u;
    return base64encode[index];
}

/** Get the second base 64 digit of a block of 4.
  * @param a The first byte of the source block of 3.
  * @param b The second byte of the source block of 3.
  * @return A base 64 digit. */
static char get1(unsigned char a, unsigned char b) {
    int const indexA = (a & 3) << 4u;
    int const indexB = b >> 4u;
    int const index = indexA | indexB;
    return base64encode[index];
}

/** Get the third base 64 digit of a block of 4.
  * @param b The second byte of the source block of 3.
  * @param c The third byte of the source block of 3.
  * @return A base 64 digit. */
static char get2(unsigned char b, unsigned char c) {
    int const indexB = (b & 15) << 2u;
    int const indexC = c >> 6u;
    int const index = indexB | indexC;
    return base64encode[index];
}

/** Get the fourth base 64 digit of a block of 4.
  * @param c The third byte of the source block of 3.
  * @return A base 64 digit. */
static char get3(int c) {
    int const index = c & 0x3f;
    return base64encode[index];
}

size_t base64_req_len(size_t src_len)
{
    return ((src_len + 2) / 3) * 4 + 1;
}

/* Convert a binary memory block in a base64 null-terminated string. */
size_t base64_encode(void const* src, size_t size, char* dest, size_t dsize)
{
    typedef struct { unsigned char a; unsigned char b; unsigned char c; } block_t;

    size_t req_len = base64_req_len(size);
    if (req_len > dsize) return 0;

    block_t const* block = (block_t*)src;
    size_t i = 0;
    for (; size >= sizeof(block_t); size -= sizeof(block_t), ++block) {
        dest[i++] = get0(block->a);
        dest[i++] = get1(block->a, block->b);
        dest[i++] = get2(block->b, block->c);
        dest[i++] = get3(block->c);
    }
    if (size)
    {
        dest[i++] = get0(block->a);
        if (--size) 
        {
            dest[i++] = get1(block->a, block->b);
            dest[i++] = get2(block->b, 0);
            dest[i++] = '=';
        }
        else
        {
            dest[i++] = get1(block->a, 0);
            dest[i++] = '=';
            dest[i++] = '=';
        }
    }
    dest[i] = '\0';
    return i;
}

// Convert a 8Byte binary memory block in a base64 null-terminated string. Requires dest len >= 13 bytes.
int int64_to_base64(bytemap64 b64, char* dest, size_t dsize)
{
    if (dsize < 13) return 0;

    *dest++ = get0(b64.as_bytes[0]);
    *dest++ = get1(b64.as_bytes[0], b64.as_bytes[1]);
    *dest++ = get2(b64.as_bytes[1], b64.as_bytes[2]);
    *dest++ = get3(b64.as_bytes[2]);

    *dest++ = get0(b64.as_bytes[3]);
    *dest++ = get1(b64.as_bytes[3], b64.as_bytes[4]);
    *dest++ = get2(b64.as_bytes[4], b64.as_bytes[5]);
    *dest++ = get3(b64.as_bytes[5]);

    *dest++ = get0(b64.as_bytes[6]);
    *dest++ = get1(b64.as_bytes[6], b64.as_bytes[7]);
    *dest++ = get2(b64.as_bytes[7], 0);

    *dest++ = '=';
    *dest = '\0';
    return 12;
}

/** Lookup table that converts a base64 digit to integer. */
static char const base64decode[] = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 65, 64, 64,
    64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
    64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

/** Escape values. */
enum special_e {
    notabase64 = 64, /**< Value to return when a non base64 digit is found. */
    terminator = 65, /**< Value to return when the character '=' is found.  */
};

// Convert a base64 null-terminated string to binary format.
size_t base64_decode(char const* src, void* dest, size_t dsize)
{
    unsigned char const* s = (unsigned char*)src;
    char* p = dest;
    size_t len = 0;
    for (;; s++) {
        unsigned char const a = base64decode[*s];
        if (a == notabase64) break;
        if (a == terminator) break;

        unsigned char const b = base64decode[*++s];
        if (b == notabase64) return 0;
        if (b == terminator) return 0;

        if (len >= dsize) return -1;
        p[len++] = ((a << 2) | (b >> 4)) & 0xFF;

        unsigned char const c = base64decode[*++s];
        if (c == notabase64) return 0;

        unsigned char const d = base64decode[*++s];
        if (d == notabase64) return 0;

        if (c == terminator)
            if (d == terminator)
                break;
            else
                return 0;

        if (len >= dsize) return -1;
        p[len++] = ((b << 4) | (c >> 2)) & 0xFF;

        if (d == terminator) break;
        p[len++] = ((c << 6) | (d >> 0)) & 0xFF;
    }
    return len;
}

// Convert a base64 null-terminated string to 8Byte binary memory block in.
// Returns 1 if success
int base64_to_bin64(const char* src, bytemap64 *out_b64)
{
    size_t len = base64_decode(src, out_b64, sizeof(*out_b64));
    return (len == 8);
}
