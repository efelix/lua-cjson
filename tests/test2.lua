#!/usr/bin/env lua

-- Lua CJSON tests
--
-- Mark Pulford <mark@kyne.com.au>
--
-- Note: The output of this script is easier to read with "less -S"
package.cpath = package.cpath .. ";" .. '..\\x64\\Release\\' .. '?.dll'


local json = require "lua_cjson"
local json_safe = require "lua_cjson.safe"
local util = require "util"

local function json_encode_output_type(value)
    local text = json.encode(value)
    if string.match(text, "{.*}") then
        return "object"
    elseif string.match(text, "%[.*%]") then
        return "array"
    else
        return "scalar"
    end
end

local function gen_raw_octets()
    local chars = {}
    for i = 0, 255 do chars[i + 1] = string.char(i) end
    return table.concat(chars)
end

-- Generate every UTF-16 codepoint, including supplementary codes
local function gen_utf16_escaped()
    -- Create raw table escapes
    local utf16_escaped = {}
    local count = 0

    local function append_escape(code)
        local esc = ('\\u%04X'):format(code)
        table.insert(utf16_escaped, esc)
    end

    table.insert(utf16_escaped, '"')
    for i = 0, 0xD7FF do
        append_escape(i)
    end
    -- Skip 0xD800 - 0xDFFF since they are used to encode supplementary
    -- codepoints
    for i = 0xE000, 0xFFFF do
        append_escape(i)
    end
    -- Append surrogate pair for each supplementary codepoint
    for high = 0xD800, 0xDBFF do
        for low = 0xDC00, 0xDFFF do
            append_escape(high)
            append_escape(low)
        end
    end
    table.insert(utf16_escaped, '"')

    return table.concat(utf16_escaped)
end

function load_testdata()
    local data = {}

    -- Data for 8bit raw <-> escaped octets tests
    data.octets_raw = gen_raw_octets()
    data.octets_escaped = util.file_load("octets-escaped.dat")

    -- Data for \uXXXX -> UTF-8 test
    data.utf16_escaped = gen_utf16_escaped()

    -- Load matching data for utf16_escaped
    local utf8_loaded
    utf8_loaded, data.utf8_raw = pcall(util.file_load, "utf8.dat")
    if not utf8_loaded then
        data.utf8_raw = "Failed to load utf8.dat - please run genutf8.pl"
    end

    data.table_cycle = {}
    data.table_cycle[1] = data.table_cycle

    local big = {}
    for i = 1, 1100 do
        big = { { 10, false, true, json.null }, "string", a = big }
    end
    data.deeply_nested_data = big

    return data
end

function test_decode_cycle(filename)
    local obj1 = json.decode(util.file_load(filename))
    local obj2 = json.decode(json.encode(obj1))
    return util.compare_values(obj1, obj2)
end

-- Set up data used in tests
local Inf = math.huge;
local NaN = math.huge * 0;

local testdata = load_testdata()

local cjson_tests = {
    -- Test API variables
    { "Check module name, version",
      function () return json._NAME, json._VERSION end, { },
      true, { "lua_cjson", "2.1.1.QUIKSHARP" } },

    { "Set encode_number_precision(16)",
      json.encode_number_precision, { 16 }, true, { 16 } },
	  
    -- Test decoding simple types
    { "Decode number",
      json.encode, { 1/3 }, true, { "0.33333333" } },

    { "Decode number",
      json.encode, { 0.33 }, true, { "0.33" } },

    { "Decode number",
      json.encode, { 1.5 }, true, { "1.5" } },

    { "Decode number",
      json.encode, { 10.0 }, true, { "10.0" } },

    { "Decode number",
      json.encode, { 100200300.123 }, true, { "100200300.123" } },

}

print(("==> Testing Lua CJSON version %s\n"):format(json._VERSION))

util.run_test_group(cjson_tests)

for _, filename in ipairs(arg) do
    util.run_test("Decode cycle " .. filename, test_decode_cycle, { filename },
                  true, { true })
end

local pass, total = util.run_test_summary()


if pass == total then
    print("==> Summary: all tests succeeded")
else
    print(("==> Summary: %d/%d tests failed"):format(total - pass, total))
    os.exit(1)
end

-- vi:ai et sw=4 ts=4:
