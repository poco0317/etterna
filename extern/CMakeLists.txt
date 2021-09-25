# Disable all warnings for this directory
# Possible CMAKE_CXX_COMPILER_ID values: https://cmake.org/cmake/help/latest/variable/CMAKE_LANG_COMPILER_ID.html
if(MSVC)
    add_compile_options(/W0)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL AppleClang)
    set_directory_properties(PROPERTIES XCODE_ATTRIBUTE_GCC_WARN_INHIBIT_ALL_WARNINGS YES)
else() # All remaining compilers
    add_compile_options(-w)
endif()

# Library Options
set(BUILD_SHARED_LIBS OFF) # Tell CMake to prefer static libs

# External Libraries
## Curl Specific Options
set(CMAKE_USE_LIBSSH2 OFF CACHE BOOL "" FORCE) # Disable curl libssh2
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)    # Tell curl not to build standalone binary
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)     # Disable curl testing
set(CMAKE_USE_OPENSSL ON CACHE BOOL "" FORCE)  # Require OpenSSL
set(HTTP_ONLY ON CACHE BOOL "" FORCE)          # Disable all protocols except HTTP/S
add_subdirectory(curl)

## SQLiteCpp
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE) # Disable SQLiteCpp static analysis
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)  # Disable SQLiteCpp lintiPng
add_subdirectory(SQLiteCpp)

## websocketpp
add_subdirectory(asio-1.12.2) # Only required for websocketpp
add_subdirectory(websocketpp)

## discord-rpc
set(CLANG_FORMAT_CMD OFF CACHE BOOL "" FORCE)   # Disable discord clang-format
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)     # Disable discord examples
add_subdirectory(discord-rpc)

# Poco Libraries
set(FORCE_OPENSSL ON CACHE BOOL "" FORCE)      # Require OpenSSL
set(ENABLE_JWT ON CACHE BOOL "" FORCE)         # Need JWT
set(ENABLE_MONGODB OFF CACHE BOOL "" FORCE)    # Dont need MongoDB
set(ENABLE_REDIS OFF CACHE BOOL "" FORCE)      # Dont need Redis
set(ENABLE_XML OFF CACHE BOOL "" FORCE)        # Dont need XML for now
set(ENABLE_DATA_MYSQL OFF CACHE BOOL "" FORCE) # Dont need MySQL
set(ENABLE_DATA_POSTGRESQL OFF CACHE BOOL "" FORCE) # Dont need PostgreSQL
set(ENABLE_DATA_ODBC OFF CACHE BOOL "" FORCE)
set(ENABLE_APACHECONNECTOR OFF CACHE BOOL "" FORCE) # Dont need ApacheConnector
set(CMAKE_PREFIX_PATH ${PROJECT_SOURCE_DIR}/extern/poco)
add_subdirectory(poco)

## Header Only
add_subdirectory(rapidjson)
add_subdirectory(stb)

## Remaining Libraries
add_subdirectory(luajit)
add_subdirectory(muFFT)
add_subdirectory(glew-1.5.8)
add_subdirectory(pcre)
add_subdirectory(zlib)
add_subdirectory(mad-0.15.1b)
add_subdirectory(fmt)
add_subdirectory(plog)
add_subdirectory(boost/nowide)
add_subdirectory(filesystem)

## Vorbis depends on ogg
add_subdirectory(newogg)
add_subdirectory(newvorbis)
target_link_libraries(vorbis ogg)

# Only enable crashpad if the user wants it.
if(WITH_CRASHPAD)
    add_subdirectory(crashpad)
endif()

if(MSVC) # Only required on Windows
    add_subdirectory(ffmpeg)
endif()

## IDE Folder Organization
list(APPEND EXT_TARGETS
    SQLiteCpp sqlite3
    discord-rpc muFFT
    zlib libmad pcre
    ogg vorbis glfw
    libcurl fmt)

list(APPEND LUA_TARGETS luajit libluajit minilua buildvm)

## Sort all extern libraries to a IDE folder/filter
set_property(TARGET ${EXT_TARGETS} PROPERTY FOLDER "External Libraries")
set_property(TARGET ${LUA_TARGETS} PROPERTY FOLDER "External Libraries/Lua 5.1")
