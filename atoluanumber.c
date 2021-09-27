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
#include "lua_cjson.h"

enum special_c {
    space = ' ',
    zero = '0',
    nine = '9',
    minus = '-',
    plus = '+',
    tab = '\t',
};

#define white_space(c) ((c) == space || (c) == tab)
#define valid_digit10(c) ((c) >= zero && (c) <= nine)
#define valid_digit16(c) ( ((c) >= zero && (c) <= nine) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))

static double const e_mul[] =
{
    1.0, 1E1, 1E2, 1E3, 1E4, 1E5, 1E6, 1E7, 1E8, 1E9, 1E10, 1E11, 1E12, 1E13, 1E14, 1E15, 1E16, 1E17, 1E18, 1E19,
    1E20, 1E21, 1E22, 1E23, 1E24, 1E25, 1E26, 1E27, 1E28, 1E29, 1E30, 1E31, 1E32, 1E33, 1E34, 1E35, 1E36, 1E37, 1E38, 1E39,
    1E40, 1E41, 1E42, 1E43, 1E44, 1E45, 1E46, 1E47, 1E48, 1E49,
};


/* JSON numbers should take the following form:
 *      -?(0|[1-9]|[1-9][0-9]+)(.[0-9]+)?([eE][-+]?[0-9]+)?
 *
 * json_next_number_token() allows other forms:
 * - numbers starting with '+'
 * - NaN, -NaN, infinity, -infinity
 * - hexadecimal numbers
 * - numbers with leading zeros
 *
 * if json_strict_rule is != 0, JSON nubers will be strict JSON rules.
 */

// Converts string to token value (int64 or double). returns 1 on success or 0 on fail
int json_next_number_token(json_parse_t* json, json_token_t* token)
{
    int json_strict_rule = !json->cfg->decode_invalid_numbers;
    const char* p = json->ptr;

    int is_neg = 0;
    int base = 10;
    int is_int = 0;
    long int_value = 0;
    int is_double = 0;

    /* Skip minus sign if it exists */
    if (*p == '-')
    {
        p++;
        is_neg = 1;
    }
    else
        if (*p == '+') 
        {
            if (json_strict_rule)
                return 0; // * Reject numbers starting with + */
            p++;
        }

    if (*p == '0')
    {
        p++;
        char c = *p | 0x20;
        if (c == 'x')
        {
            if (json_strict_rule)
                return 0; // * Reject numbers starting with 0x, or leading zeros */
            base = 16;
            p++;
        }
        else
        {
            if (json_strict_rule && *p == '0')
                return 0; // * Reject numbers starting with 0x, or leading zeros */
            // we do not decode ord values, just skip leading zeroes
            while (*p == '0') p++;
            if ((*p == '.') || (c == 'e'))
                is_double = 1;
        }
    }
    else
    {
        // check for nan and inf
        if (((p[0] == 'n') || (p[0] == 'N')) && ((p[1] == 'a') || (p[1] == 'A')) && ((p[2] == 'n') || (p[2] == 'N')))
        {
            if (json_strict_rule)
                return 0; /* Reject inf/nan */

            p += 3;
            token->type = T_NUMBER;
            token->value.number = NAN;
            json->ptr = p;     /* Skip the processed number */
            return 1;
        }
        if (((p[0] == 'i') || (p[0] == 'I')) && ((p[1] == 'n') || (p[1] == 'N')) && ((p[2] == 'f') || (p[2] == 'F')))
        {
            if (json_strict_rule)
                return 0; /* Reject inf/nan */

            p += 3;
            if (strncasecmp(p, "inity", 5) == 0) p += 5;
            token->type = T_NUMBER;
            token->value.number = (is_neg) ? -INFINITY : INFINITY;
            json->ptr = p;     /* Skip the processed number */
            return 1;
        }
    }

    if (!is_double)
    {
        const char* b = p;
        if (base == 10)
        {
            // Get digits before decimal point or exponent, if any.
            while (valid_digit10(*p))
            {
                int_value = int_value * 10 + (*p - zero);
                p++;
            }

            if (*p == '.' || *p == 'e' || *p == 'E')
                is_double = 1;
            else
                is_int = (b != p);
        }
        else if (base == 16)
        {
            for (;; p++) {
                if (valid_digit10(*p))
                    int_value = (int_value << 4) + (*p - zero);
                else
                {
                    char c = *p | 0x20;
                    if ((c >= 'a') && (c <= 'f'))
                        int_value = (int_value << 4) + (c - 'a' + 10);
                    else break;
                }
            }
            is_int = (b != p);
        }
    }

    if (is_int)
    {
        token->value.integer = (is_neg) ? -int_value : int_value;
        token->type = T_INTEGER;
        json->ptr = p;     /* Skip the processed number */
        return 1;
    }

    if (is_double && (base == 10))
    {
        double double_value = int_value;
        // Get digits after decimal point, if any.
        if (*p == '.') {
            double pow10 = 10.0;
            p++;
            while (valid_digit10(*p)) {
                double_value += (*p - zero) / pow10;
                pow10 *= 10.0;
                p++;
            }
        }
        // Handle exponent, if any.
        int frac = 0;
        double scale = 1.0;
        if ((*p == 'e') || (*p == 'E')) {
            // Get sign of exponent, if any.
            p += 1;
            if (*p == minus) {
                frac = 1;
                p++;
            }
            else
                if (*p == plus)
                    p++;

            // Get digits of exponent, if any.
            unsigned int expon;
            for (expon = 0U; valid_digit10(*p); p++) {
                expon = expon * 10U + (*p - (char)zero);
            }
            if (expon > 308U) expon = 308U;

            // Calculate scaling factor.
            while (expon >= 50U) { scale *= 1E50; expon -= 50U; }
            if (expon > 0U) scale *= e_mul[expon];
        }

        // Return signed and scaled floating point result.
        double_value = (frac ? (double_value / scale) : (double_value * scale));
        token->value.number = (is_neg) ? -double_value : double_value;
        token->type = T_NUMBER;
        json->ptr = p;     /* Skip the processed number */
        return 1;
    }
    return 0;
}

