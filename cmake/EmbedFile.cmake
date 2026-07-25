# Embeds a file into a C++ header as a byte array.
# Usage: cmake -DINPUT=<file> -DOUTPUT=<header> -DVAR=<name> -P EmbedFile.cmake
file(READ "${INPUT}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} — do not edit.
#pragma once
#include <string_view>

namespace mxldl::webui
{
    inline constexpr unsigned char k${VAR}[] = {${bytes}};

    inline std::string_view ${VAR}()
    {
        return {reinterpret_cast<char const*>(k${VAR}), sizeof(k${VAR})};
    }
}
")
