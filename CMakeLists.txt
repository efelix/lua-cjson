# Copyright (C) 2007-2011 LuaDist.
# Created by Peter Kapec
# Redistribution and use of this file is allowed according to the terms of the MIT license.
# For details see the COPYRIGHT file distributed with LuaDist.
# Please note that the package source code is licensed under its own license.

project ( lua-cjson C )
cmake_minimum_required ( VERSION 2.6 )
include ( dist.cmake )


# lua-cjson modules
add_definitions ( -DVERSION="1.0.3" )
install_lua_module( cjson lua_cjson.c strbuf.c )


# Install Lua-CJSON Documentation
install_data( README NEWS performance.txt rfc4627.txt )

# Install Tests
install_test( tests/ )