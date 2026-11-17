#!/usr/bin/env python3
"""Run host Lua tests using an interpreter, or a system Lua 5.3/5.4 shared library."""
import ctypes
import ctypes.util
import shutil
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit('usage: run_lua.py script.lua')
for name in ('lua5.1', 'lua5.2', 'lua5.3', 'lua5.4', 'lua'):
    binary = shutil.which(name)
    if binary:
        raise SystemExit(subprocess.call([binary, sys.argv[1]]))
for name in ('lua5.4', 'lua5.3'):
    library = ctypes.util.find_library(name)
    if library:
        break
else:
    raise SystemExit('Install Lua 5.1+ (or liblua5.3/liblua5.4) to run tests')
lua = ctypes.CDLL(library)
lua.luaL_newstate.restype = ctypes.c_void_p
lua.luaL_openlibs.argtypes = [ctypes.c_void_p]
lua.luaL_loadfilex.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
lua.lua_pcallk.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ssize_t, ctypes.c_void_p]
lua.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
lua.lua_tolstring.restype = ctypes.c_char_p
lua.lua_close.argtypes = [ctypes.c_void_p]
state = lua.luaL_newstate()
if not state:
    raise SystemExit('Could not allocate Lua state')
try:
    lua.luaL_openlibs(state)
    code = lua.luaL_loadfilex(state, sys.argv[1].encode(), None)
    if not code:
        code = lua.lua_pcallk(state, 0, 0, 0, 0, None)
    if code:
        print(lua.lua_tolstring(state, -1, None).decode(), file=sys.stderr)
        raise SystemExit(1)
finally:
    lua.lua_close(state)
