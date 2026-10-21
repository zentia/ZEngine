#include "Runtime/Function/ShaderLab/ShaderLabSurface.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace ZEngine::ShaderLab
{
    namespace
    {

        constexpr size_t kNpos = std::string::npos;

        std::string Trim(const std::string& s)
        {
            auto not_space = [](unsigned char c) { return !std::isspace(c); };
            auto begin = std::find_if(s.begin(), s.end(), not_space);
            auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
            if (begin >= end)
            {
                return {};
            }
            return std::string(begin, end);
        }

        std::string ToLower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool IsIdentChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        bool IsSpaceChar(char c)
        {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        // Blanks out comments and string/char literals, preserving length and
        // newline positions so offsets into the masked copy stay valid offsets
        // into the original.
        //
        // Same idea as the masker in ShaderLabSemanticRewrite.cpp; kept local
        // because both TUs need index-preserving text surgery and neither
        // wants to expose an internal helper through a public header.
        std::string MaskNonCode(const std::string& src)
        {
            std::string out(src.size(), ' ');
            const size_t n = src.size();
            size_t i = 0;
            while (i < n)
            {
                if (src[i] == '\n' || src[i] == '\r')
                {
                    out[i] = src[i];
                    ++i;
                    continue;
                }
                if (src[i] == '/' && i + 1 < n && src[i + 1] == '/')
                {
                    while (i < n && src[i] != '\n' && src[i] != '\r')
                    {
                        ++i;
                    }
                    continue;
                }
                if (src[i] == '/' && i + 1 < n && src[i + 1] == '*')
                {
                    i += 2;
                    while (i < n)
                    {
                        if (src[i] == '*' && i + 1 < n && src[i + 1] == '/')
                        {
                            i += 2;
                            break;
                        }
                        if (src[i] == '\n' || src[i] == '\r')
                        {
                            out[i] = src[i];
                        }
                        ++i;
                    }
                    continue;
                }
                if (src[i] == '"' || src[i] == '\'')
                {
                    const char quote = src[i];
                    ++i;
                    while (i < n)
                    {
                        if (src[i] == '\\' && i + 1 < n)
                        {
                            i += 2;
                            continue;
                        }
                        if (src[i] == quote)
                        {
                            ++i;
                            break;
                        }
                        // An unterminated literal is a lexer problem, not
                        // ours; stop at the line end so we do not blank the
                        // rest of the file.
                        if (src[i] == '\n')
                        {
                            break;
                        }
                        ++i;
                    }
                    continue;
                }
                out[i] = src[i];
                ++i;
            }
            return out;
        }

        size_t SkipBlanks(const std::string& s, size_t from)
        {
            while (from < s.size() && IsSpaceChar(s[from]))
            {
                ++from;
            }
            return from;
        }

        // Position of `needle` as a whole identifier inside `s`, at or after
        // `from`. Empty needle would match everywhere, so it is rejected.
        size_t FindWholeWord(const std::string& s, const std::string& needle, size_t from)
        {
            if (needle.empty())
            {
                return kNpos;
            }
            size_t at = s.find(needle, from);
            while (at != kNpos)
            {
                const bool left_ok = (at == 0) || !IsIdentChar(s[at - 1]);
                const size_t end = at + needle.size();
                const bool right_ok = (end >= s.size()) || !IsIdentChar(s[end]);
                if (left_ok && right_ok)
                {
                    return at;
                }
                at = s.find(needle, at + 1);
            }
            return kNpos;
        }

        size_t MatchingDelimiter(const std::string& s, size_t open_pos, char open, char close)
        {
            int depth = 0;
            for (size_t i = open_pos; i < s.size(); ++i)
            {
                if (s[i] == open)
                {
                    ++depth;
                }
                else if (s[i] == close)
                {
                    --depth;
                    if (depth == 0)
                    {
                        return i;
                    }
                }
            }
            return kNpos;
        }

        // Splits `[begin, end)` on top-level commas. Angle brackets are not
        // tracked: `Foo<A, B>` does not occur in a surface shader signature.
        std::vector<std::string> SplitTopLevel(const std::string& s, size_t begin, size_t end)
        {
            std::vector<std::string> parts;
            int depth = 0;
            size_t start = begin;
            for (size_t i = begin; i < end; ++i)
            {
                const char c = s[i];
                if (c == '(' || c == '[')
                {
                    ++depth;
                }
                else if (c == ')' || c == ']')
                {
                    --depth;
                }
                else if (c == ',' && depth == 0)
                {
                    parts.push_back(Trim(s.substr(start, i - start)));
                    start = i + 1;
                }
            }
            const std::string tail = Trim(s.substr(start, end - start));
            if (!tail.empty())
            {
                parts.push_back(tail);
            }
            return parts;
        }

        std::vector<std::string> Tokenize(const std::string& s)
        {
            std::vector<std::string> tokens;
            std::istringstream stream(s);
            std::string token;
            while (stream >> token)
            {
                tokens.push_back(token);
            }
            return tokens;
        }

        bool IsParamQualifier(const std::string& token)
        {
            const std::string lowered = ToLower(token);
            return lowered == "in" || lowered == "out" || lowered == "inout" ||
                   lowered == "const" || lowered == "uniform";
        }

        // `inout SurfaceOutputStandard o` -> "SurfaceOutputStandard".
        // Drops parameter qualifiers and the trailing parameter name.
        std::string ParameterTypeOf(const std::string& parameter)
        {
            std::vector<std::string> tokens = Tokenize(parameter);
            std::vector<std::string> kept;
            for (const std::string& token : tokens)
            {
                if (!IsParamQualifier(token))
                {
                    kept.push_back(token);
                }
            }
            if (kept.size() >= 2)
            {
                kept.pop_back();  // parameter name
            }
            if (kept.empty())
            {
                return {};
            }
            std::string type = kept.front();
            for (size_t i = 1; i < kept.size(); ++i)
            {
                type += " " + kept[i];
            }
            return type;
        }

        // Struct members are SEMICOLON-separated. Splitting on commas -- the
        // obvious thing to reach for, since that is what a parameter list
        // uses -- leaves the ';' glued to the member name, so `float2
        // uv_MainTex;` parses as a member called "uv_MainTex;" and the
        // generated code asks for a `MainTex;_ST` uniform.
        std::vector<std::string> SplitDeclarations(const std::string& s, size_t begin, size_t end)
        {
            std::vector<std::string> parts;
            int depth = 0;
            size_t start = begin;
            for (size_t i = begin; i < end; ++i)
            {
                const char c = s[i];
                if (c == '{' || c == '(' || c == '[')
                {
                    ++depth;
                }
                else if (c == '}' || c == ')' || c == ']')
                {
                    --depth;
                }
                else if (c == ';' && depth == 0)
                {
                    parts.push_back(Trim(s.substr(start, i - start)));
                    start = i + 1;
                }
            }
            const std::string tail = Trim(s.substr(start, end - start));
            if (!tail.empty())
            {
                parts.push_back(tail);
            }
            return parts;
        }

        struct SurfaceSignature
        {
            bool found = false;
            std::string input_type;
            std::string output_type;
        };

        // Reads the two-parameter type list off the surface function. The
        // `Input` struct is NOT assumed to be named "Input" -- Unity lets the
        // author call it anything, so the only reliable source is the
        // signature itself.
        SurfaceSignature FindSurfaceSignature(const std::string& masked, const std::string& function)
        {
            SurfaceSignature out;
            size_t from = 0;
            while (true)
            {
                const size_t at = FindWholeWord(masked, function, from);
                if (at == kNpos)
                {
                    break;
                }
                const size_t paren = SkipBlanks(masked, at + function.size());
                if (paren < masked.size() && masked[paren] == '(')
                {
                    const size_t close = MatchingDelimiter(masked, paren, '(', ')');
                    if (close != kNpos)
                    {
                        const std::vector<std::string> params =
                            SplitTopLevel(masked, paren + 1, close);
                        if (params.size() >= 2)
                        {
                            out.found = true;
                            out.input_type = ParameterTypeOf(params[0]);
                            out.output_type = ParameterTypeOf(params[1]);
                            return out;
                        }
                    }
                }
                from = at + function.size();
            }
            return out;
        }

        // ---- `Input` members -------------------------------------------------

        enum class InputMemberKind
        {
            Uv,           // uv_MainTex   -> TEXCOORD0 * _MainTex_ST
            Uv2,          // uv2_MainTex  -> TEXCOORD1 * _MainTex_ST
            WorldPos,
            WorldNormal,
            ViewDir,
            ScreenPos,
            Unsupported   // Unity would fill it; ZEngine does not know how yet
        };

        struct InputMember
        {
            std::string type;
            std::string name;
            InputMemberKind kind = InputMemberKind::Unsupported;
            std::string texture;  // Uv / Uv2 only
        };

        // `uv_<X>` / `uv2_<X>` name a TEXTURE PROPERTY, and Unity property
        // names carry the leading underscore: `uv_MainTex` means the sampler
        // `_MainTex`, whose tiling/offset uniform is `_MainTex_ST`. The member
        // name drops that underscore (`uv_` + `MainTex`), so put it back --
        // unless the author already wrote `uv__MainTex`.
        std::string TexturePropertyOf(const std::string& suffix)
        {
            if (!suffix.empty() && suffix[0] != '_')
            {
                return "_" + suffix;
            }
            return suffix;
        }

        InputMemberKind ClassifyMember(const std::string& name, std::string& texture_out)
        {
            if (name.rfind("uv2_", 0) == 0 && name.size() > 4)
            {
                texture_out = TexturePropertyOf(name.substr(4));
                return InputMemberKind::Uv2;
            }
            if (name.rfind("uv_", 0) == 0 && name.size() > 3)
            {
                texture_out = TexturePropertyOf(name.substr(3));
                return InputMemberKind::Uv;
            }
            if (name == "worldPos")
            {
                return InputMemberKind::WorldPos;
            }
            if (name == "worldNormal")
            {
                return InputMemberKind::WorldNormal;
            }
            if (name == "viewDir")
            {
                return InputMemberKind::ViewDir;
            }
            if (name == "screenPos")
            {
                return InputMemberKind::ScreenPos;
            }
            return InputMemberKind::Unsupported;
        }

        std::vector<InputMember> FindStructMembers(const std::string& src,
                                                  const std::string& masked,
                                                  const std::string& struct_name)
        {
            std::vector<InputMember> members;

            size_t from = 0;
            size_t open = kNpos;
            size_t close = kNpos;

            while (true)
            {
                const size_t kw = FindWholeWord(masked, "struct", from);
                if (kw == kNpos)
                {
                    break;
                }
                const size_t id_begin = SkipBlanks(masked, kw + 6);
                size_t id_end = id_begin;
                while (id_end < masked.size() && IsIdentChar(masked[id_end]))
                {
                    ++id_end;
                }
                const std::string id = masked.substr(id_begin, id_end - id_begin);
                if (id == struct_name)
                {
                    const size_t brace = SkipBlanks(masked, id_end);
                    if (brace < masked.size() && masked[brace] == '{')
                    {
                        open = brace;
                        close = MatchingDelimiter(masked, brace, '{', '}');
                        break;
                    }
                }
                from = kw + 6;
            }

            if (open == kNpos || close == kNpos)
            {
                return members;
            }

            const std::vector<std::string> declarations = SplitDeclarations(masked, open + 1, close);
            for (const std::string& declaration : declarations)
            {
                // Drop any semantic annotation (`: TEXCOORD0`) and array suffix.
                std::string text = declaration;
                const size_t colon = text.find(':');
                if (colon != std::string::npos)
                {
                    text = text.substr(0, colon);
                }
                const size_t bracket = text.find('[');
                if (bracket != std::string::npos)
                {
                    text = text.substr(0, bracket);
                }

                std::vector<std::string> tokens = Tokenize(text);
                if (tokens.size() < 2)
                {
                    continue;
                }

                InputMember member;
                member.name = tokens.back();
                tokens.pop_back();
                std::string type = tokens.front();
                for (size_t i = 1; i < tokens.size(); ++i)
                {
                    type += " " + tokens[i];
                }
                member.type = type;
                member.kind = ClassifyMember(member.name, member.texture);
                members.push_back(member);
            }

            (void)src;
            return members;
        }

        bool BodyIncludesFile(const std::string& body, const std::string& file_name)
        {
            std::istringstream stream(body);
            std::string line;
            while (std::getline(stream, line))
            {
                const std::string trimmed = Trim(line);
                if (trimmed.rfind("#", 0) != 0)
                {
                    continue;
                }
                const size_t hash = trimmed.find('#');
                std::istringstream line_stream(trimmed.substr(hash + 1));
                std::string directive;
                std::string operand;
                line_stream >> directive >> operand;
                if (ToLower(directive) != "include")
                {
                    continue;
                }
                // operand is `"UnityCG.cginc"` or `<UnityCG.cginc>`.
                std::string path = operand;
                path.erase(std::remove(path.begin(), path.end(), '"'), path.end());
                path.erase(std::remove(path.begin(), path.end(), '<'), path.end());
                path.erase(std::remove(path.begin(), path.end(), '>'), path.end());
                const size_t slash = path.find_last_of("/\\");
                if (slash != std::string::npos)
                {
                    path = path.substr(slash + 1);
                }
                if (ToLower(path) == ToLower(file_name))
                {
                    return true;
                }
            }
            return false;
        }

        bool BodyDeclaresName(const std::string& masked, const std::string& name)
        {
            // A declaration is what we care about, but any occurrence is good
            // enough here: the only reason to look is to avoid emitting a
            // duplicate global.
            return FindWholeWord(masked, name, 0) != kNpos;
        }

    }  // namespace

    bool SurfaceShaderDirective::HasOption(const std::string& option) const
    {
        const std::string wanted = ToLower(option);
        return std::find(options.begin(), options.end(), wanted) != options.end();
    }

    SurfaceShaderDirective ParseSurfaceDirective(const std::string& program_source)
    {
        SurfaceShaderDirective out;

        std::istringstream stream(program_source);
        std::string line;
        while (std::getline(stream, line))
        {
            std::vector<std::string> tokens = Tokenize(Trim(line));
            if (tokens.size() < 2)
            {
                continue;
            }

            // Accept `#pragma surface ...` and `# pragma surface ...`.
            size_t index = 0;
            std::string head = tokens[0];
            if (head == "#" && tokens.size() >= 3)
            {
                head = tokens[1];
                index = 1;
            }
            else if (!head.empty() && head[0] == '#')
            {
                head = head.substr(1);
            }
            else
            {
                continue;
            }

            if (ToLower(head) != "pragma")
            {
                continue;
            }

            if (index + 1 >= tokens.size() || ToLower(tokens[index + 1]) != "surface")
            {
                continue;
            }

            if (index + 2 >= tokens.size())
            {
                out.error = "'#pragma surface' needs at least a surface function name";
                return out;
            }

            out.function = tokens[index + 2];

            // Unity: omitting the lighting model means Lambert.
            out.model_name = (index + 3 < tokens.size()) ? tokens[index + 3] : "Lambert";
            const std::string model_lower = ToLower(out.model_name);
            if (model_lower == "standard")
            {
                out.model = SurfaceLightingModel::Standard;
            }
            else if (model_lower == "lambert")
            {
                out.model = SurfaceLightingModel::Lambert;
            }
            else if (model_lower == "blinnphong")
            {
                out.model = SurfaceLightingModel::BlinnPhong;
            }
            else
            {
                out.model = SurfaceLightingModel::Unknown;
                out.error = "unsupported surface lighting model '" + out.model_name +
                            "' (supported: Standard, Lambert, BlinnPhong)";
                return out;
            }

            for (size_t i = index + 4; i < tokens.size(); ++i)
            {
                // Allow a trailing comment: `... addshadow // comment`.
                if (tokens[i].rfind("//", 0) == 0)
                {
                    break;
                }
                out.options.push_back(ToLower(tokens[i]));
            }

            out.valid = true;
            return out;
        }

        return out;  // no `#pragma surface`: valid == false, error empty
    }

    SurfaceGenerateResult GenerateSurfacePasses(const std::string& program_source,
                                                const SurfaceShaderDirective& directive)
    {
        SurfaceGenerateResult out;

        if (!directive.valid)
        {
            out.error = directive.error.empty() ? "no '#pragma surface' directive" : directive.error;
            return out;
        }

        const std::string masked = MaskNonCode(program_source);

        const SurfaceSignature signature = FindSurfaceSignature(masked, directive.function);
        if (!signature.found)
        {
            out.error = "surface function '" + directive.function +
                        "' not found, or it does not take two parameters";
            return out;
        }
        if (signature.input_type.empty() || signature.output_type.empty())
        {
            out.error = "could not read the parameter types of '" + directive.function + "'";
            return out;
        }

        // The output struct is the authority on which lighting data the author
        // writes to; the pragma's model decides the lighting math. They have to
        // agree, or we would generate a fragment shader that reads fields the
        // struct does not have.
        const bool output_is_standard = (signature.output_type == "SurfaceOutputStandard");
        const bool output_is_legacy = (signature.output_type == "SurfaceOutput");
        if (!output_is_standard && !output_is_legacy)
        {
            out.error = "surface function '" + directive.function + "' writes to '" +
                        signature.output_type +
                        "', which is not SurfaceOutput or SurfaceOutputStandard";
            return out;
        }

        const bool model_wants_standard = (directive.model == SurfaceLightingModel::Standard);
        if (model_wants_standard != output_is_standard)
        {
            out.error = "lighting model '" + directive.model_name + "' expects inout " +
                        (model_wants_standard ? "SurfaceOutputStandard" : "SurfaceOutput") +
                        ", but '" + directive.function + "' declares inout " +
                        signature.output_type;
            return out;
        }

        std::vector<InputMember> members =
            FindStructMembers(program_source, masked, signature.input_type);
        if (members.empty())
        {
            out.warnings.push_back("could not read any member of '" + signature.input_type +
                                   "'; the generated vertex shader will not fill it");
        }

        // Texture tiling/offset uniforms (`_MainTex_ST`) that Unity declares
        // implicitly for every `uv_<Texture>` member.
        std::vector<std::string> st_uniforms;
        for (const InputMember& member : members)
        {
            if (member.kind != InputMemberKind::Uv && member.kind != InputMemberKind::Uv2)
            {
                continue;
            }
            const std::string st = member.texture + "_ST";
            if (std::find(st_uniforms.begin(), st_uniforms.end(), st) == st_uniforms.end() &&
                !BodyDeclaresName(masked, st))
            {
                st_uniforms.push_back(st);
            }
        }

        for (const InputMember& member : members)
        {
            if (member.kind == InputMemberKind::Unsupported)
            {
                out.warnings.push_back("Input member '" + member.name +
                                       "' is not filled: ZEngine does not know how to "
                                       "generate it yet (it will read as zero)");
            }
        }

        // ---- varying layout -------------------------------------------------
        // worldPos / worldNormal are unconditional: the lighting math needs
        // them even when `Input` does not ask for them.
        struct VaryingField
        {
            std::string type;
            std::string name;
            std::string assignment;  // vertex-shader RHS
            std::string source;      // "texcoord" / "texcoord1" / generated
        };

        std::vector<VaryingField> varyings;
        int texcoord_index = 0;

        for (const InputMember& member : members)
        {
            if (member.kind == InputMemberKind::Uv)
            {
                VaryingField field;
                field.type = "float2";
                field.name = member.name;
                field.assignment = "v.texcoord * " + member.texture + "_ST.xy + " +
                                   member.texture + "_ST.zw";
                varyings.push_back(field);
            }
            else if (member.kind == InputMemberKind::Uv2)
            {
                VaryingField field;
                field.type = "float2";
                field.name = member.name;
                field.assignment = "v.texcoord1 * " + member.texture + "_ST.xy + " +
                                   member.texture + "_ST.zw";
                varyings.push_back(field);
            }
        }

        VaryingField world_pos;
        world_pos.type = "float3";
        world_pos.name = "zWorldPos";
        varyings.push_back(world_pos);

        VaryingField world_normal;
        world_normal.type = "float3";
        world_normal.name = "zWorldNormal";
        varyings.push_back(world_normal);

        bool need_screen_pos = false;
        for (const InputMember& member : members)
        {
            if (member.kind == InputMemberKind::ScreenPos)
            {
                need_screen_pos = true;
            }
        }
        if (need_screen_pos)
        {
            VaryingField screen_pos;
            screen_pos.type = "float4";
            screen_pos.name = "zScreenPos";
            varyings.push_back(screen_pos);
        }

        // A surface shader file never includes UnityCG itself -- Unity's
        // surface compiler pulls it in. It has to go BEFORE the body, not
        // after: the body uses `fixed4`, `tex2D` and friends, and macro
        // expansion happens in source order, so an include appended at the
        // end would come too late for all of them.
        std::string preamble;
        if (!BodyIncludesFile(program_source, "UnityCG.cginc"))
        {
            preamble = "#include \"UnityCG.cginc\"\n";
        }

        // Same argument as the include: the tiling/offset uniforms go in front
        // of the body, so a shader that reads `_MainTex_ST` in its own `surf`
        // finds it declared.
        for (const std::string& st : st_uniforms)
        {
            preamble += "float4 " + st + ";\n";
        }

        // ---- build the common preamble --------------------------------------
        std::ostringstream code;
        code << "\n";
        code << "// ------------------------------------------------------------------\n";
        code << "// Generated by ZEngine's surface-shader desugaring for\n";
        code << "//   #pragma surface " << directive.function << " " << directive.model_name;
        for (const std::string& option : directive.options)
        {
            code << " " << option;
        }
        code << "\n";
        code << "// ------------------------------------------------------------------\n";

        code << "\n";
        code << "struct ZSurfaceVertexInput\n";
        code << "{\n";
        code << "    float4 vertex    : POSITION;\n";
        code << "    float3 normal    : NORMAL;\n";
        code << "    float4 tangent   : TANGENT;\n";
        code << "    float2 texcoord  : TEXCOORD0;\n";
        code << "    float2 texcoord1 : TEXCOORD1;\n";
        code << "};\n";
        code << "\n";
        code << "struct ZSurfaceVarying\n";
        code << "{\n";
        code << "    float4 zClipPos : SV_POSITION;\n";
        for (VaryingField& field : varyings)
        {
            field.source = "TEXCOORD" + std::to_string(texcoord_index);
            code << "    " << field.type << " " << field.name << " : " << field.source << ";\n";
            ++texcoord_index;
        }
        code << "};\n";

        // ---- vertex shader ---------------------------------------------------
        code << "\n";
        code << "ZSurfaceVarying zSurf_vert (ZSurfaceVertexInput v)\n";
        code << "{\n";
        code << "    ZSurfaceVarying o;\n";
        code << "    o.zClipPos = UnityObjectToClipPos(v.vertex);\n";
        code << "    const float4 zWorldPos4 = mul(unity_ObjectToWorld, v.vertex);\n";
        code << "    o.zWorldPos = zWorldPos4.xyz;\n";
        code << "    o.zWorldNormal = UnityObjectToWorldDir(v.normal);\n";
        for (const VaryingField& field : varyings)
        {
            if (field.name == "zWorldPos" || field.name == "zWorldNormal")
            {
                continue;
            }
            if (field.name == "zScreenPos")
            {
                code << "    o.zScreenPos = o.zClipPos;\n";
                continue;
            }
            code << "    o." << field.name << " = " << field.assignment << ";\n";
        }
        code << "    return o;\n";
        code << "}\n";

        // ---- shared fragment prologue ---------------------------------------
        auto emit_surf_setup = [&](std::ostringstream& target) {
            target << "    " << signature.input_type << " zSurfIN;\n";
            for (const InputMember& member : members)
            {
                switch (member.kind)
                {
                    case InputMemberKind::Uv:
                    case InputMemberKind::Uv2:
                        target << "    zSurfIN." << member.name << " = IN." << member.name << ";\n";
                        break;
                    case InputMemberKind::WorldPos:
                        // Unity hands `worldPos` over unnormalised too; it is
                        // an interpolation, so renormalising here would be a
                        // silent behaviour difference.
                        target << "    zSurfIN." << member.name << " = IN.zWorldPos;\n";
                        break;
                    case InputMemberKind::WorldNormal:
                        target << "    zSurfIN." << member.name << " = IN.zWorldNormal;\n";
                        break;
                    case InputMemberKind::ViewDir:
                        target << "    zSurfIN." << member.name
                               << " = normalize(_WorldSpaceCameraPos - IN.zWorldPos);\n";
                        break;
                    case InputMemberKind::ScreenPos:
                        target << "    zSurfIN." << member.name << " = IN.zScreenPos;\n";
                        break;
                    case InputMemberKind::Unsupported:
                        // Deliberately left unwritten: the parser already
                        // warned about it.
                        break;
                }
            }
            target << "\n";
            target << "    " << signature.output_type << " o;\n";
            if (output_is_standard)
            {
                target << "    o.Albedo = float3(0.0, 0.0, 0.0);\n";
                target << "    o.Normal = float3(0.0, 0.0, 1.0);\n";
                target << "    o.Emission = float3(0.0, 0.0, 0.0);\n";
                target << "    o.Metallic = 0.0;\n";
                target << "    o.Smoothness = 0.0;\n";
                target << "    o.Occlusion = 1.0;\n";
                target << "    o.Alpha = 1.0;\n";
            }
            else
            {
                target << "    o.Albedo = float3(0.0, 0.0, 0.0);\n";
                target << "    o.Normal = float3(0.0, 0.0, 1.0);\n";
                target << "    o.Emission = float3(0.0, 0.0, 0.0);\n";
                target << "    o.Specular = 0.0;\n";
                target << "    o.Gloss = 0.0;\n";
                target << "    o.Alpha = 1.0;\n";
            }
            target << "\n";
            target << "    " << directive.function << " (zSurfIN, o);\n";
        };

        // ---- forward (lighting) pass ----------------------------------------
        {
            std::ostringstream pass_code;
            pass_code << code.str();
            pass_code << "\n";
            pass_code << "float4 zSurf_frag (ZSurfaceVarying IN) : SV_Target\n";
            pass_code << "{\n";
            emit_surf_setup(pass_code);
            pass_code << "\n";
            pass_code << "    const float3 zN = normalize(IN.zWorldNormal);\n";
            pass_code << "    const float3 zL = normalize(_WorldSpaceLightPos0.xyz);\n";
            pass_code << "    const float zNdL = saturate(dot(zN, zL));\n";

            if (directive.model == SurfaceLightingModel::Standard)
            {
                // A deliberately simplified metallic/roughness approximation.
                // It is NOT Unity's Standard BRDF -- ZEngine has no GI, no
                // light list and no BRDF data yet. What matters structurally is
                // that every field the author wrote is read.
                pass_code << "    const float3 zV = normalize(_WorldSpaceCameraPos - IN.zWorldPos);\n";
                pass_code << "    const float3 zH = normalize(zL + zV);\n";
                pass_code << "    const float zNdH = saturate(dot(zN, zH));\n";
                pass_code << "    const float zSpecPower = exp2(10.0 * o.Smoothness + 1.0);\n";
                pass_code << "    const float3 zSpecColor = lerp(float3(0.04, 0.04, 0.04), o.Albedo, o.Metallic);\n";
                pass_code << "    const float3 zDiffuseColor = o.Albedo * (1.0 - o.Metallic);\n";
                pass_code << "    float3 zColor = zDiffuseColor * _LightColor0.rgb * zNdL;\n";
                pass_code << "    zColor += zSpecColor * _LightColor0.rgb * pow(zNdH, zSpecPower) * zNdL;\n";
                pass_code << "    zColor *= o.Occlusion;\n";
                pass_code << "    zColor += o.Emission;\n";
            }
            else if (directive.model == SurfaceLightingModel::BlinnPhong)
            {
                pass_code << "    const float3 zV = normalize(_WorldSpaceCameraPos - IN.zWorldPos);\n";
                pass_code << "    const float3 zH = normalize(zL + zV);\n";
                pass_code << "    const float zNdH = saturate(dot(zN, zH));\n";
                pass_code << "    float3 zColor = o.Albedo * _LightColor0.rgb * zNdL;\n";
                pass_code << "    zColor += _LightColor0.rgb * o.Specular * pow(zNdH, o.Gloss * 128.0 + 1.0) * zNdL;\n";
                pass_code << "    zColor += o.Emission;\n";
            }
            else  // Lambert
            {
                pass_code << "    float3 zColor = o.Albedo * _LightColor0.rgb * zNdL;\n";
                pass_code << "    zColor += o.Emission;\n";
            }

            pass_code << "    return float4(zColor, o.Alpha);\n";
            pass_code << "}\n";

            SurfacePass pass;
            pass.name = "ForwardBase";
            pass.light_mode = "ForwardBase";
            pass.vertex_entry = "zSurf_vert";
            pass.fragment_entry = "zSurf_frag";
            pass.source_code = preamble + program_source + pass_code.str();
            out.passes.push_back(std::move(pass));
        }

        // ---- shadow caster (only when asked for) -----------------------------
        //
        // `addshadow` is the option that means "also give me a ShadowCaster
        // pass". `fullforwardshadows` does not: it only changes how the
        // forward-add pass handles shadows.
        if (directive.HasOption("addshadow"))
        {
            std::ostringstream pass_code;
            pass_code << code.str();
            pass_code << "\n";
            pass_code << "// Shadow caster: ZEngine has no shadow pipeline wired to ShaderLab\n";
            pass_code << "// yet, so this writes depth only. `surf` is still called so that a\n";
            pass_code << "// cutout shader can later drive `clip()` from o.Alpha.\n";
            pass_code << "float4 zSurf_shadowFrag (ZSurfaceVarying IN) : SV_Target\n";
            pass_code << "{\n";
            emit_surf_setup(pass_code);
            pass_code << "    return float4(0.0, 0.0, 0.0, o.Alpha);\n";
            pass_code << "}\n";

            SurfacePass pass;
            pass.name = "ShadowCaster";
            pass.light_mode = "ShadowCaster";
            pass.vertex_entry = "zSurf_vert";
            pass.fragment_entry = "zSurf_shadowFrag";
            pass.source_code = preamble + program_source + pass_code.str();
            out.passes.push_back(std::move(pass));
        }

        out.ok = true;
        return out;
    }

}  // namespace ZEngine::ShaderLab
