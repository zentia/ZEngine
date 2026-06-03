# Generate engine_log.h from bq_log_category_config.ini
# This script generates a BqLog category log wrapper class

# Parameters passed via -D:
#   CONFIG_FILE - path to bq_log_category_config.ini
#   OUTPUT_DIR - output directory for generated files
#   CLASS_NAME - name of the generated class (default: engine_log)

if(NOT DEFINED CONFIG_FILE)
    message(FATAL_ERROR "CONFIG_FILE not defined")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR not defined")
endif()
if(NOT DEFINED CLASS_NAME)
    set(CLASS_NAME "engine_log")
endif()

# Read the config file
if(NOT EXISTS "${CONFIG_FILE}")
    message(FATAL_ERROR "Config file not found: ${CONFIG_FILE}")
endif()

file(READ "${CONFIG_FILE}" CONFIG_CONTENT)

# Parse categories - each line is a category name
string(REPLACE "\r\n" "\n" CONFIG_CONTENT "${CONFIG_CONTENT}")
string(REPLACE "\r" "\n" CONFIG_CONTENT "${CONFIG_CONTENT}")
string(REGEX REPLACE "\n+" ";" CATEGORY_LIST "${CONFIG_CONTENT}")

# Filter out empty entries and count categories
set(CATEGORIES)
foreach(cat IN LISTS CATEGORY_LIST)
    string(STRIP "${cat}" cat)
    if(NOT "${cat}" STREQUAL "")
        list(APPEND CATEGORIES "${cat}")
    endif()
endforeach()

list(LENGTH CATEGORIES CATEGORY_COUNT)
math(EXPR TOTAL_COUNT "${CATEGORY_COUNT} + 1")  # +1 for empty root category

message(STATUS "[Precompile] Generating ${CLASS_NAME}.h with ${CATEGORY_COUNT} categories")

# Build the category names array
set(NAMES_ARRAY "            \"\"")
set(CAT_INDEX 1)
foreach(cat IN LISTS CATEGORIES)
    string(APPEND NAMES_ARRAY "\n            , \"${cat}\"")
    math(EXPR CAT_INDEX "${CAT_INDEX} + 1")
endforeach()

# Build the category struct definitions
set(CAT_STRUCTS "")
set(CAT_INDEX 1)
foreach(cat IN LISTS CATEGORIES)
    string(APPEND CAT_STRUCTS "        struct EBCO : public ${CLASS_NAME}_category_base<${CAT_INDEX}> {\n        } ${cat};    //${cat}\n")
    math(EXPR CAT_INDEX "${CAT_INDEX} + 1")
endforeach()

# Generate the header file content
set(HEADER_CONTENT "#pragma once
// clang-format off
/*
 * Copyright (C) 2024 Tencent.
 * BQLOG is licensed under the Apache License, Version 2.0.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an \"AS IS\" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
/*!
 * Generated Wrapper For ${CLASS_NAME}
 *
 * This is a category_log that supports attaching a category to each log entry.
 * Categories can be used to filter logs within the appender settings.
 *
 *  Usage: 
 *  bq::${CLASS_NAME} my_category_log = bq::${CLASS_NAME}::create_log(log_name, log_config);  //create a ${CLASS_NAME} object with config.
 *  my_category_log.info(\"content\");  //this is for empty category
 *  my_category_log.info(my_category_log.cat.ZEngine, \"content\"); //this is a log entry for category ZEngine
 */

#include \"bq_log/bq_log.h\"


namespace bq {
    class ${CLASS_NAME} : public category_log
    {
    private:
        template<uint32_t CAT_INDEX>
        struct ${CLASS_NAME}_category_base : public bq::log_category_base<CAT_INDEX> {};

        struct ${CLASS_NAME}_category_config
        {
            const char* names[${TOTAL_COUNT}] = {
${NAMES_ARRAY}
            };
        };

        struct EBCO ${CLASS_NAME}_category_root
        {
${CAT_STRUCTS}        };

        template<typename T>
        struct ${CLASS_NAME}_category_root_holder
        {
            static ${CLASS_NAME}_category_config config_;
            static ${CLASS_NAME}_category_root root_;
        };

    public:
        const ${CLASS_NAME}_category_root& cat = ${CLASS_NAME}_category_root_holder<void>::root_;

    protected:
        template<typename STR>
        struct is_${CLASS_NAME}_format_type
        {
            static constexpr bool value = bq::tools::_is_bq_log_format_type<STR>::value;
        };

    private:
        ${CLASS_NAME}() : category_log(){}
        ${CLASS_NAME}(const log& child_inst) : category_log(child_inst){}

    public:
        /// <summary>
        /// Create a ${CLASS_NAME} object
        /// </summary>
        /// <param name=\"log_name\">If the log name is an empty string, bqLog will automatically assign you a unique log name. If the log name already exists, it will return the previously existing log object and overwrite the previous configuration with the new config.</param>
        /// <param name=\"config_content\">Log config string</param>
        /// <returns>A ${CLASS_NAME} object, if create failed, the is_valid() method of it will return false</returns>
        static ${CLASS_NAME} create_log(const bq::string& log_name, const bq::string& config_content);

        /// <summary>
        /// Get a ${CLASS_NAME} object by it's name
        /// </summary>
        /// <param name=\"log_name\">Name of the ${CLASS_NAME} object you want to find</param>
        /// <returns>A ${CLASS_NAME} object, if the ${CLASS_NAME} object with specific name was not found, the is_valid() method of it will return false</returns>
        static ${CLASS_NAME} get_log_by_name(const bq::string& log_name);

        using log::verbose;
        using log::debug;
        using log::info;
        using log::warning;
        using log::error;
        using log::fatal;

        ///Core log functions with category param, there are 6 log levels:
        ///verbose, debug, info, warning, error, fatal
        template<typename STR, uint32_t CAT_INDEX>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> verbose(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const;
        template<typename STR, uint32_t CAT_INDEX, typename...Args>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> verbose(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const;
        template<typename STR, uint32_t CAT_INDEX>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> debug(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const;
        template<typename STR, uint32_t CAT_INDEX, typename...Args>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> debug(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const;
        template<typename STR, uint32_t CAT_INDEX>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> info(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const;
        template<typename STR, uint32_t CAT_INDEX, typename...Args>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> info(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const;
        template<typename STR, uint32_t CAT_INDEX>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> warning(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const;
        template<typename STR, uint32_t CAT_INDEX, typename...Args>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> warning(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const;
        template<typename STR, uint32_t CAT_INDEX>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> error(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const;
        template<typename STR, uint32_t CAT_INDEX, typename...Args>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> error(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const;
        template<typename STR, uint32_t CAT_INDEX>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> fatal(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const;
        template<typename STR, uint32_t CAT_INDEX, typename...Args>
        bq::enable_if_t<is_${CLASS_NAME}_format_type<STR>::value, bool> fatal(const ${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const;
    };

    template<typename T>
    ${CLASS_NAME}::${CLASS_NAME}_category_root ${CLASS_NAME}::${CLASS_NAME}_category_root_holder<T>::root_;
    template<typename T>
    ${CLASS_NAME}::${CLASS_NAME}_category_config ${CLASS_NAME}::${CLASS_NAME}_category_root_holder<T>::config_;

    inline ${CLASS_NAME} ${CLASS_NAME}::create_log(const bq::string& log_name, const bq::string& config_content)
    {
        uint64_t log_id = api::__api_create_log(log_name.c_str(), config_content.c_str(), ${TOTAL_COUNT}, ${CLASS_NAME}_category_root_holder<void>::config_.names);
        log result = get_log_by_id(log_id);
        return result;
    }
    
    inline ${CLASS_NAME} ${CLASS_NAME}::get_log_by_name(const bq::string& log_name)
    {
        ${CLASS_NAME} result = log::get_log_by_name(log_name);
        if (!result.is_valid())
        {
            return result;
        }
        //check categories
        if (result.get_categories_count() != ${TOTAL_COUNT})
        {
            return ${CLASS_NAME}();
        }
        for (size_t i = 0; i < result.get_categories_count(); ++i)
        {
            if (result.get_categories_name_array()[i] != ${CLASS_NAME}_category_root_holder<void>::config_.names[i])
            {
                return ${CLASS_NAME}();
            }
        }
        return result;
    }

    template<typename STR, uint32_t CAT_INDEX>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::verbose(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::verbose, log_content);
    }
    template<typename STR, uint32_t CAT_INDEX, typename...Args>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::verbose(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::verbose, log_format_content, args...);
    }
    template<typename STR, uint32_t CAT_INDEX>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::debug(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::debug, log_content);
    }
    template<typename STR, uint32_t CAT_INDEX, typename...Args>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::debug(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::debug, log_format_content, args...);
    }
    template<typename STR, uint32_t CAT_INDEX>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::info(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::info, log_content);
    }
    template<typename STR, uint32_t CAT_INDEX, typename...Args>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::info(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::info, log_format_content, args...);
    }
    template<typename STR, uint32_t CAT_INDEX>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::warning(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::warning, log_content);
    }
    template<typename STR, uint32_t CAT_INDEX, typename...Args>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::warning(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::warning, log_format_content, args...);
    }
    template<typename STR, uint32_t CAT_INDEX>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::error(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::error, log_content);
    }
    template<typename STR, uint32_t CAT_INDEX, typename...Args>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::error(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::error, log_format_content, args...);
    }
    template<typename STR, uint32_t CAT_INDEX>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::fatal(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_content) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::fatal, log_content);
    }
    template<typename STR, uint32_t CAT_INDEX, typename...Args>
    inline bq::enable_if_t<${CLASS_NAME}::is_${CLASS_NAME}_format_type<STR>::value, bool> ${CLASS_NAME}::fatal(const ${CLASS_NAME}::${CLASS_NAME}_category_base<CAT_INDEX>& category, const STR& log_format_content, const Args&... args) const
    {
        (void)category;
        return do_log(CAT_INDEX, log_level::fatal, log_format_content, args...);
    }
}
// clang-format on
")

# Ensure output directory exists
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

# Write the file
set(OUTPUT_FILE "${OUTPUT_DIR}/${CLASS_NAME}.h")
file(WRITE "${OUTPUT_FILE}" "${HEADER_CONTENT}")

message(STATUS "[Precompile] Generated ${OUTPUT_FILE}")
