#include "ShaderCbufferReflection.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

namespace ZEngine::ShaderLab
{
    namespace
    {
        constexpr uint32_t k_RegisterSize = 16;

        uint32_t RoundUp(uint32_t value, uint32_t align)
        {
            if (align <= 1)
            {
                return value;
            }
            return (value + align - 1) / align * align;
        }

        /// 把注释换成空格（保留换行，免得两行的标识符被粘在一起）。
        std::string StripComments(const std::string& source)
        {
            std::string out;
            out.reserve(source.size());

            for (size_t i = 0; i < source.size();)
            {
                if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/')
                {
                    while (i < source.size() && source[i] != '\n')
                    {
                        out.push_back(' ');
                        ++i;
                    }
                    continue;
                }
                if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*')
                {
                    out.push_back(' ');
                    out.push_back(' ');
                    i += 2;
                    while (i < source.size() && !(source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/'))
                    {
                        out.push_back(source[i] == '\n' ? '\n' : ' ');
                        ++i;
                    }
                    if (i < source.size())
                    {
                        out.push_back(' ');
                        out.push_back(' ');
                        i += 2;
                    }
                    continue;
                }
                out.push_back(source[i]);
                ++i;
            }
            return out;
        }

        /// 把预处理指令整行抹掉，并**连带抹掉条件编译区间里的代码**。
        ///
        /// 为什么连区间一起抹：`#ifdef _EMISSION_ON` 里的属性到底在不在、在哪个
        /// 偏移，取决于编译时的 keyword 集合。解析器不知道变体，瞎猜一个 offset
        /// 比查不到更糟 —— 查不到是 SetFloat 返回 false，猜错是静默写坏别人的
        /// 属性。所以条件区间整体跳过，等以后接上变体信息再按变体反射。
        ///
        /// 已知限制（非 TODO，是取舍）：条件编译里的属性当前不可用。
        std::string StripPreprocessorRegions(const std::string& source)
        {
            std::string out;
            out.reserve(source.size());

            // 条件编译嵌套深度。`#if` 进入时 +1，`#endif` 退出时 -1。
            int cond_depth = 0;

            size_t i = 0;
            while (i < source.size())
            {
                const size_t eol = source.find('\n', i);
                const size_t end = (eol == std::string::npos) ? source.size() : eol;
                const std::string line = source.substr(i, end - i);

                const size_t hash = line.find_first_not_of(" \t\r");
                const bool is_directive = hash != std::string::npos && line[hash] == '#';

                std::string directive;
                if (is_directive)
                {
                    size_t cursor = hash + 1;
                    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t'))
                    {
                        ++cursor;
                    }
                    const size_t begin = cursor;
                    while (cursor < line.size() && std::isalpha(static_cast<unsigned char>(line[cursor])))
                    {
                        ++cursor;
                    }
                    directive = line.substr(begin, cursor - begin);
                }

                if (directive == "if" || directive == "ifdef" || directive == "ifndef")
                {
                    ++cond_depth;
                }
                else if (directive == "endif")
                {
                    if (cond_depth > 0)
                    {
                        --cond_depth;
                    }
                }

                // 指令行自己也要抹；`#endif` 先把 depth 减回 0，但它是指令，
                // 靠 is_directive 抹掉。`#else` / `#elif` 不影响 depth。
                const bool blank = is_directive || cond_depth > 0;
                for (const char c : line)
                {
                    out.push_back(blank && c != '\n' ? ' ' : c);
                }
                if (eol != std::string::npos)
                {
                    out.push_back('\n');
                }

                i = (eol == std::string::npos) ? source.size() : eol + 1;
            }
            return out;
        }

        bool IsIdentifierChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        /// 从声明里剥出数组长度：`_Foo[4]` -> ("_Foo", 4)。没有下标返回 1。
        uint32_t SplitArraySuffix(std::string& name)
        {
            const size_t bracket = name.find('[');
            if (bracket == std::string::npos)
            {
                return 1;
            }

            uint32_t count = 1;
            const size_t close = name.find(']', bracket);
            if (close != std::string::npos)
            {
                const std::string digits = name.substr(bracket + 1, close - bracket - 1);
                int parsed = 0;
                for (const char c : digits)
                {
                    if (c < '0' || c > '9')
                    {
                        parsed = 0;
                        break;
                    }
                    parsed = parsed * 10 + (c - '0');
                }
                if (parsed > 0)
                {
                    count = static_cast<uint32_t>(parsed);
                }
            }
            name = name.substr(0, bracket);
            return count;
        }

        /// 类型字符串 -> 分量数 / 标量字节数 / 是否矩阵。
        ///
        /// HLSL 的 `float4x4` 是 **行 x 列**，GLSL 的 `mat4x3` 是 **列 x 行** ——
        /// 命名约定正好相反，非方阵时 size 会算错，所以这里分开认。
        bool ParseType(const std::string& type,
                       uint32_t& out_components,
                       uint32_t& out_scalar_size,
                       bool& out_is_matrix,
                       uint32_t& out_matrix_rows,
                       uint32_t& out_matrix_cols)
        {
            out_components = 0;
            out_scalar_size = 4;
            out_is_matrix = false;
            out_matrix_rows = 0;
            out_matrix_cols = 0;

            if (type.empty())
            {
                return false;
            }

            // ---- 矩阵 ----
            const size_t x_pos = type.find('x');
            if (x_pos != std::string::npos && x_pos > 0 && x_pos + 1 < type.size())
            {
                const std::string head = type.substr(0, x_pos);
                const std::string tail = type.substr(x_pos + 1);

                auto leading_digits = [](const std::string& s, size_t& out_value) -> bool
                {
                    size_t i = s.size();
                    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9')
                    {
                        --i;
                    }
                    if (i == s.size())
                    {
                        return false;
                    }
                    out_value = static_cast<size_t>(std::stoul(s.substr(i)));
                    return true;
                };

                size_t head_n = 0;
                size_t tail_n = 0;
                if (!leading_digits(head, head_n) || !leading_digits(tail, tail_n))
                {
                    return false;
                }
                if (head_n < 2 || head_n > 4 || tail_n < 2 || tail_n > 4)
                {
                    return false;
                }

                out_is_matrix = true;
                if (type.compare(0, 3, "mat") == 0)
                {
                    // GLSL: matCxR -> C 列 R 行
                    out_matrix_cols = static_cast<uint32_t>(head_n);
                    out_matrix_rows = static_cast<uint32_t>(tail_n);
                }
                else
                {
                    // HLSL: floatRxC -> R 行 C 列
                    out_matrix_rows = static_cast<uint32_t>(head_n);
                    out_matrix_cols = static_cast<uint32_t>(tail_n);
                }
                return true;
            }

            // ---- GLSL 方阵 mat2 / mat3 / mat4 ----
            if (type.compare(0, 3, "mat") == 0 && type.size() == 4 && type[3] >= '2' && type[3] <= '4')
            {
                out_is_matrix = true;
                out_matrix_cols = static_cast<uint32_t>(type[3] - '0');
                out_matrix_rows = out_matrix_cols;
                return true;
            }

            // ---- 标量 / 向量 ----
            // 尾部的数字就是分量数。把尾数剥掉剩下的必须是认识的基础类型。
            size_t last_alpha = type.size();
            while (last_alpha > 0 && type[last_alpha - 1] >= '0' && type[last_alpha - 1] <= '9')
            {
                --last_alpha;
            }
            const std::string base = type.substr(0, last_alpha);
            const std::string digits = type.substr(last_alpha);

            if (!digits.empty())
            {
                out_components = static_cast<uint32_t>(std::stoul(digits));
                if (out_components < 2 || out_components > 4)
                {
                    return false;
                }
            }
            else
            {
                out_components = 1;
            }

            if (base == "float" || base == "half" || base == "fixed" || base == "int" || base == "uint" ||
                base == "bool" || base == "vec" || base == "ivec" || base == "uvec" || base == "bvec" ||
                base == "min16float" || base == "min10float" || base == "snorm" || base == "unorm")
            {
                out_scalar_size = 4;
                return true;
            }
            if (base == "double" || base == "dvec")
            {
                out_scalar_size = 8;
                return true;
            }
            return false;
        }

        struct Declaration
        {
            std::string name;
            uint32_t components {0};
            uint32_t scalar_size {4};
            uint32_t array_count {1};
            bool is_matrix {false};
            uint32_t matrix_rows {0};
            uint32_t matrix_cols {0};
        };

        std::vector<std::string> SplitWords(const std::string& text)
        {
            std::vector<std::string> words;
            size_t i = 0;
            while (i < text.size())
            {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                {
                    ++i;
                }
                const size_t begin = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
                {
                    ++i;
                }
                if (i > begin)
                {
                    words.push_back(text.substr(begin, i - begin));
                }
            }
            return words;
        }

        bool IsModifier(const std::string& word)
        {
            return word == "const" || word == "static" || word == "row_major" || word == "column_major" ||
                   word == "uniform" || word == "precise" || word == "extern" || word == "groupshared";
        }

        /// 解析一条 `float4 _Foo : packoffset(c0);` 这样的声明。
        ///
        /// `float4 a, b;` 这种一次声明多个变量的写法只取**第一个**名字：cbuffer
        /// 里极少见，而且全取会让 a、b 分到不同 offset 却共用同一个类型，容易
        /// 误判。认不全（表现为 SetFloat("b") 返回 false）好过算错。
        bool ParseDeclaration(const std::string& statement, Declaration& out)
        {
            // 丢掉 `: ...` 尾巴（packoffset / register / 语义）。
            std::string body = statement;
            const size_t colon = body.find(':');
            if (colon != std::string::npos)
            {
                body = body.substr(0, colon);
            }

            // 按逗号拆开（`float4 a, b;`），再把每片按空白拆成词，摊平成一条词流：
            // "float4 a" + " b" -> ["float4", "a", "b"]。
            std::vector<std::string> words;
            {
                std::string current;
                for (const char c : body)
                {
                    if (c == ',')
                    {
                        for (std::string& word : SplitWords(current))
                        {
                            words.push_back(std::move(word));
                        }
                        current.clear();
                        continue;
                    }
                    current.push_back(c);
                }
                for (std::string& word : SplitWords(current))
                {
                    words.push_back(std::move(word));
                }
            }

            size_t index = 0;
            while (index < words.size() && IsModifier(words[index]))
            {
                ++index;
            }
            if (index >= words.size())
            {
                return false;
            }
            const std::string type = words[index];
            ++index;
            if (index >= words.size())
            {
                return false;
            }

            // `_Curve[4]` 先剥下标再截标识符；`_Foo=0` 截到 '=' 前。
            std::string raw_name = words[index];
            const uint32_t array_count = SplitArraySuffix(raw_name);
            size_t name_end = 0;
            while (name_end < raw_name.size() && IsIdentifierChar(raw_name[name_end]))
            {
                ++name_end;
            }
            if (name_end == 0)
            {
                return false;
            }
            const std::string name = raw_name.substr(0, name_end);
            if (name[0] >= '0' && name[0] <= '9')
            {
                return false;
            }

            out = Declaration {};
            out.name = name;
            out.array_count = array_count;
            return ParseType(type,
                             out.components,
                             out.scalar_size,
                             out.is_matrix,
                             out.matrix_rows,
                             out.matrix_cols);
        }

        /// 算出一条声明的落点，并把游标往前推。
        void PlaceDeclaration(const Declaration& decl, ShaderCbufferPacking packing, uint32_t& cursor, ShaderPropertyLayout& out)
        {
            const uint32_t element_size = decl.is_matrix
                                              ? k_RegisterSize * (packing == ShaderCbufferPacking::Std140 ? decl.matrix_cols : decl.matrix_rows)
                                              : decl.components * decl.scalar_size;

            uint32_t stride = element_size;

            if (packing == ShaderCbufferPacking::Std140)
            {
                // std140：vec2 对齐 8，vec3 / vec4 对齐 16；矩阵按列，每列 stride 16。
                uint32_t align = decl.scalar_size;
                if (decl.is_matrix)
                {
                    align = k_RegisterSize;
                }
                else if (decl.components >= 3)
                {
                    align = k_RegisterSize;
                }
                else if (decl.components == 2)
                {
                    align = 2 * decl.scalar_size;
                }
                if (decl.array_count > 1)
                {
                    stride = RoundUp(element_size, k_RegisterSize);
                }
                cursor = RoundUp(cursor, align);
            }
            else
            {
                // HLSL cbuffer：一律 4 字节起步，然后「不能跨 16 字节寄存器边界」。
                // 跨了就整体挪到下一个寄存器。float4 因此天然 16 对齐。
                cursor = RoundUp(cursor, decl.scalar_size);
                if (cursor % k_RegisterSize + element_size > k_RegisterSize)
                {
                    cursor = RoundUp(cursor, k_RegisterSize);
                }
                stride = decl.array_count > 1 ? RoundUp(element_size, k_RegisterSize) : element_size;
            }

            out.name = decl.name;
            out.offset = cursor;
            out.size = stride * decl.array_count;
            out.components = decl.is_matrix ? decl.matrix_rows : decl.components;
            out.array_count = decl.array_count;
            out.element_size = element_size;
            out.is_matrix = decl.is_matrix;

            cursor += out.size;
        }

        /// 从 pos 起读一个标识符（跳过前导空白）。读不到返回空串。
        std::string ReadIdentifier(const std::string& source, size_t& pos)
        {
            while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r' ||
                                           source[pos] == '\n'))
            {
                ++pos;
            }
            const size_t begin = pos;
            while (pos < source.size() && IsIdentifierChar(source[pos]))
            {
                ++pos;
            }
            if (pos == begin)
            {
                return {};
            }
            return source.substr(begin, pos - begin);
        }

        /// 三种常量缓冲写法：
        ///   - `CBUFFER_START(名字) ... CBUFFER_END`       —— Unity 宏
        ///   - `cbuffer 名字 { ... }`                      —— HLSL
        ///   - `uniform 名字 { ... };`                     —— GLSL / SPIR-V 的
        ///     uniform block，通常前面还带 `layout(std140, binding = 0)`
        enum class BlockKind
        {
            Macro,     // CBUFFER_START(...) ... CBUFFER_END
            Cbuffer,   // cbuffer Name { ... }
            Uniform    // [layout(...)] uniform Name { ... };
        };

        /// 找到指定名字的常量缓冲块体范围。
        /// 找到返回 true，并把 [out_begin, out_end) 设成块体。名字不匹配的块会跳过
        /// 继续往后找（一个 shader 里通常有 UnityPerDraw / UnityPerMaterial 好几个；
        /// GLSL 里 `uniform sampler2D _Foo;` 这类非块声明也会被自动跳过）。
        bool FindCbufferBlock(const std::string& source,
                              const std::string& buffer_name,
                              size_t search_from,
                              size_t& out_body_begin,
                              size_t& out_body_end)
        {
            static constexpr std::pair<const char*, BlockKind> k_keywords[] = {
                {"CBUFFER_START", BlockKind::Macro},
                {"cbuffer", BlockKind::Cbuffer},
                {"uniform", BlockKind::Uniform},
            };

            size_t keyword = std::string::npos;
            BlockKind kind = BlockKind::Macro;
            for (const auto& candidate : k_keywords)
            {
                const size_t found = source.find(candidate.first, search_from);
                if (found != std::string::npos && (keyword == std::string::npos || found < keyword))
                {
                    keyword = found;
                    kind = candidate.second;
                }
            }
            if (keyword == std::string::npos)
            {
                return false;
            }

            // 这个块不匹配时，从关键字往后挪一格继续找下一个候选。
            const size_t next_search = keyword + 1;
            auto skip_this_block = [&]()
            {
                return FindCbufferBlock(source, buffer_name, next_search, out_body_begin, out_body_end);
            };

            size_t body_begin = std::string::npos;
            bool body_is_brace = false;

            if (kind == BlockKind::Macro)
            {
                size_t cursor = keyword + std::strlen("CBUFFER_START");
                while (cursor < source.size() && source[cursor] != '(')
                {
                    if (source[cursor] == ';' || source[cursor] == '{')
                    {
                        return skip_this_block();
                    }
                    ++cursor;
                }
                if (cursor >= source.size())
                {
                    return false;
                }
                ++cursor;  // 跳过 '('
                const std::string name = ReadIdentifier(source, cursor);
                if (name.empty())
                {
                    return skip_this_block();
                }
                while (cursor < source.size() && source[cursor] != ')')
                {
                    ++cursor;
                }
                if (cursor >= source.size())
                {
                    return false;
                }
                if (name != buffer_name)
                {
                    return skip_this_block();
                }
                body_begin = cursor + 1;
                body_is_brace = false;  // 宏形式以 CBUFFER_END 收尾
            }
            else
            {
                size_t cursor = keyword + std::strlen(kind == BlockKind::Cbuffer ? "cbuffer" : "uniform");
                const std::string name = ReadIdentifier(source, cursor);
                if (name.empty())
                {
                    return skip_this_block();
                }
                // 允许 `cbuffer Name : register(b0) {` 与
                // `layout(std140, binding = 0) uniform Name {` 两种前缀。
                // 遇到 `;` 或 `}` 说明这不是个块（`uniform sampler2D _Foo;`），跳过。
                while (cursor < source.size() && source[cursor] != '{')
                {
                    if (source[cursor] == ';' || source[cursor] == '}')
                    {
                        return skip_this_block();
                    }
                    ++cursor;
                }
                if (cursor >= source.size())
                {
                    return false;
                }
                if (name != buffer_name)
                {
                    return skip_this_block();
                }
                body_begin = cursor + 1;
                body_is_brace = true;
            }

            if (!body_is_brace)
            {
                const size_t end_marker = source.find("CBUFFER_END", body_begin);
                if (end_marker == std::string::npos)
                {
                    return false;
                }
                out_body_begin = body_begin;
                out_body_end = end_marker;
                return true;
            }

            // 花括号配对（cbuffer 里可能嵌 struct）
            int depth = 1;
            size_t i = body_begin;
            while (i < source.size() && depth > 0)
            {
                if (source[i] == '{')
                {
                    ++depth;
                }
                else if (source[i] == '}')
                {
                    --depth;
                }
                ++i;
            }
            if (depth != 0)
            {
                return false;
            }
            out_body_begin = body_begin;
            out_body_end = i - 1;
            return true;
        }
    }  // namespace

    const ShaderPropertyLayout* ShaderCbufferLayout::Find(const std::string& name) const
    {
        for (const ShaderPropertyLayout& property : properties)
        {
            if (property.name == name)
            {
                return &property;
            }
        }
        return nullptr;
    }

    ShaderCbufferLayout ReflectCbuffer(const std::string& source_code,
                                       const std::string& buffer_name,
                                       ShaderCbufferPacking packing)
    {
        ShaderCbufferLayout layout;
        layout.buffer_name = buffer_name;

        if (source_code.empty() || buffer_name.empty())
        {
            return layout;
        }

        const std::string cleaned = StripPreprocessorRegions(StripComments(source_code));

        size_t body_begin = 0;
        size_t body_end = 0;
        if (!FindCbufferBlock(cleaned, buffer_name, 0, body_begin, body_end))
        {
            return layout;
        }
        layout.found = true;

        uint32_t cursor = 0;
        size_t statement_begin = body_begin;
        int brace_depth = 0;

        for (size_t i = body_begin; i < body_end; ++i)
        {
            const char c = cleaned[i];

            // cbuffer 里嵌的 struct 整体跳过：不支持从嵌套结构里取属性。
            if (c == '{')
            {
                ++brace_depth;
                statement_begin = i + 1;
                continue;
            }
            if (c == '}')
            {
                --brace_depth;
                statement_begin = i + 1;
                continue;
            }
            if (brace_depth > 0)
            {
                continue;
            }

            if (c != ';')
            {
                continue;
            }

            const std::string statement = cleaned.substr(statement_begin, i - statement_begin);
            statement_begin = i + 1;

            Declaration decl;
            if (!ParseDeclaration(statement, decl))
            {
                continue;
            }

            // 同名重复声明（比如宏展开两次）不应覆盖第一次 —— 取第一次的落点。
            if (layout.Find(decl.name) != nullptr)
            {
                continue;
            }

            ShaderPropertyLayout property;
            PlaceDeclaration(decl, packing, cursor, property);
            layout.properties.push_back(property);
        }

        // cbuffer 的大小向上补齐到寄存器的整数倍（两套规则都一样）。
        layout.size = RoundUp(cursor, k_RegisterSize);
        return layout;
    }
}  // namespace ZEngine::ShaderLab
