# Lua 5.4 as ESP-IDF component
# Excludes lua.c (standalone interpreter) and luac.c (standalone compiler)
# Also excludes liolib.c and loslib.c (no filesystem/OS on bare metal)

set(LUA_SRCS
    src/lapi.c
    src/lauxlib.c
    src/lbaselib.c
    src/lcode.c
    src/lcorolib.c
    src/lctype.c
    src/ldblib.c
    src/ldebug.c
    src/ldo.c
    src/ldump.c
    src/lfunc.c
    src/lgc.c
    src/linit.c
    src/llex.c
    src/lmathlib.c
    src/lmem.c
    src/loadlib.c
    src/lobject.c
    src/lopcodes.c
    src/lparser.c
    src/lstate.c
    src/lstring.c
    src/lstrlib.c
    src/ltable.c
    src/ltablib.c
    src/ltm.c
    src/lundump.c
    src/lutf8lib.c
    src/lvm.c
    src/lzio.c
)

idf_component_register(
    SRCS ${LUA_SRCS}
    INCLUDE_DIRS "src"
)

# Suppress warnings in third-party Lua code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-old-style-definition
)

# Define LUA_USE_C89 to avoid POSIX dependencies
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LUA_32BITS=0
    LUA_USE_C89
)
