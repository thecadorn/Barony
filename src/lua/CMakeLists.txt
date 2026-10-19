# Vendored Lua 5.4.7, built as a plain C static library for the scripting engine.
#
# Sandboxing: the io, os, package (native module loader) and debug libraries are
# deliberately NOT compiled in (liolib.c, loslib.c, loadlib.c, ldblib.c), along with
# linit.c which references them. Scripts therefore have no way to touch the
# filesystem, spawn processes or load native code, regardless of what scripting.cpp exposes.
add_library(barony_lua STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/lapi.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lauxlib.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lbaselib.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lcode.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lcorolib.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lctype.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/ldebug.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/ldo.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/ldump.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lfunc.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lgc.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/llex.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lmathlib.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lmem.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lobject.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lopcodes.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lparser.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lstate.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lstring.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lstrlib.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/ltable.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/ltablib.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/ltm.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lundump.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lutf8lib.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lvm.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/lzio.c"
)
set_target_properties(barony_lua PROPERTIES LINKER_LANGUAGE C)
target_include_directories(barony_lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
