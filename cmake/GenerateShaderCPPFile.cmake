####################################################################################################
# This function converts any file into C/C++ source code.
# Example:
# - input file: data.dat
# - output file: data.h
# - variable name declared in output file: DATA
# - data length: sizeof(DATA)
# embed_resource("data.dat" "data.h" "DATA")
####################################################################################################

function(embed_resource resource_file_name source_file_name variable_name)

    if(EXISTS "${source_file_name}")
        if("${source_file_name}" IS_NEWER_THAN "${resource_file_name}")
            return()
        endif()
    endif()

    if(EXISTS "${resource_file_name}")
        file(READ "${resource_file_name}" hex_content HEX)

        string(REPEAT "[0-9a-f]" 32 pattern)
        string(REGEX REPLACE "(${pattern})" "\\1\n" content "${hex_content}")

        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " content "${content}")

        string(REGEX REPLACE ", $" "" content "${content}")

        # Generate the array inside a namespace for better symbol isolation, but keep a
        # backwards-compatible global alias so existing code keeps compiling.
        set(array_definition "static const std::vector<unsigned char> ${variable_name} =\n{\n${content}\n};")

        set(namespace_open "namespace Z { namespace Shader {\n")
        set(namespace_close "} } // namespace Z::Shader\n")

        set(global_alias "static const std::vector<unsigned char>& ${variable_name} = Z::Shader::${variable_name};")

        get_filename_component(file_name ${source_file_name} NAME)
        set(source "/**\n * @file ${file_name}\n * @brief Auto generated file.\n *\n * NOTE: This file declares the resource inside namespace Z::Shader and provides\n * a backwards-compatible alias in the global namespace. Call sites should be\n * migrated to `Z::Shader::${variable_name}` over time.\n */\n#include <vector>\n\n${namespace_open}${array_definition}\n${namespace_close}\n\n// Backwards-compatible alias (do not add new code that relies on the global symbol)\n${global_alias}\n")

        file(WRITE "${source_file_name}" "${source}")
    else()
        message("ERROR: ${resource_file_name} doesn't exist")
        return()
    endif()

endfunction()

# let's use it as a script
if(EXISTS "${PATH}")
    embed_resource("${PATH}" "${HEADER}" "${GLOBAL}")
endif()
