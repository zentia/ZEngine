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

        bool IsValidIdentifier(const std::string& s)
        {
            if (s.empty())
            {
                return false;
            }
            if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_')
            {
                return false;
            }
            for (const char c : s)
            {
                if (!IsIdentChar(c))
                {
                    return false;
                }
            }
            return true;
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

        // ---- function lookup ---------------------------------------------

        struct FunctionRef
        {
            bool found = false;
            std::vector<std::string> parameters;  // raw, qualifiers included
        };

        // Finds `<name> (...)` in `masked` and returns its parameter list.
        // The first hit wins, which is the declaration/definition for every
        // shape a surface shader actually uses.
        FunctionRef FindFunctionParams(const std::string& masked, const std::string& name)
        {
            FunctionRef out;
            size_t from = 0;
            while (true)
            {
                const size_t at = FindWholeWord(masked, name, from);
                if (at == kNpos)
                {
                    break;
                }
                const size_t paren = SkipBlanks(masked, at + name.size());
                if (paren < masked.size() && masked[paren] == '(')
                {
                    const size_t close = MatchingDelimiter(masked, paren, '(', ')');
                    if (close != kNpos)
                    {
                        out.found = true;
                        out.parameters = SplitTopLevel(masked, paren + 1, close);
                        return out;
                    }
                }
                from = at + name.size();
            }
            return out;
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
            const FunctionRef ref = FindFunctionParams(masked, function);
            if (ref.found && ref.parameters.size() >= 2)
            {
                out.found = true;
                out.input_type = ParameterTypeOf(ref.parameters[0]);
                out.output_type = ParameterTypeOf(ref.parameters[1]);
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

        // Same as `masked`, but with every preprocessor line blanked out.
        //
        // Needed because "does the body already declare X" must not be
        // answered from a `#pragma surface ... alphatest:_Cutoff` line: the
        // option names the variable, it does not declare it. Counting that as
        // a declaration leaves `_Cutoff` undeclared and the shader fails to
        // compile -- with an error about a variable that is plainly written
        // right there in the pragma.
        std::string MaskPreprocessorLines(const std::string& masked)
        {
            std::string out = masked;
            size_t line_begin = 0;
            while (line_begin <= out.size())
            {
                const size_t line_end = out.find('\n', line_begin);
                const size_t stop = (line_end == kNpos) ? out.size() : line_end;
                const size_t first = SkipBlanks(out, line_begin);
                if (first < stop && out[first] == '#')
                {
                    for (size_t i = line_begin; i < stop; ++i)
                    {
                        if (out[i] != '\n' && out[i] != '\r')
                        {
                            out[i] = ' ';
                        }
                    }
                }
                if (line_end == kNpos)
                {
                    break;
                }
                line_begin = line_end + 1;
            }
            return out;
        }

        bool BodyDeclaresName(const std::string& masked, const std::string& name)
        {
            // A declaration is what we care about, but any occurrence is good
            // enough here: the only reason to look is to avoid emitting a
            // duplicate global.
            return FindWholeWord(masked, name, 0) != kNpos;
        }

        // ---- surface options ---------------------------------------------
        //
        // `known && implemented`  -- ZEngine honours it (or it is genuinely a
        //                            no-op here, e.g. `nometa`: no Meta pass
        //                            is generated in the first place).
        // `known && !implemented` -- a real Unity option ZEngine ignores.
        //                            Warned about, never silently dropped.
        // `!known`                -- misspelling or something Unity added
        //                            later. Also warned about.
        struct OptionInfo
        {
            bool known = false;
            bool implemented = false;
        };

        OptionInfo ClassifyOption(const std::string& option)
        {
            const size_t colon = option.find(':');
            const std::string name = (colon == kNpos) ? option : option.substr(0, colon);

            // --- implemented (or a genuine no-op under ZEngine) -------------
            static const char* const kImplemented[] = {
                "addshadow",           // extra ShadowCaster pass
                "noforwardadd",        // suppress the ForwardAdd pass
                "noshadow",            // no shadow pass is generated anyway
                "nometa",              // no Meta pass is generated
                "nofog", "noambient", "novertexlights", "nolightmap",
                "nodynlightmap", "nodirlightmap", "noshadowmask",
                "noinstancing",        // instancing is not wired up either way
                "alpha",               // alpha blending on the forward passes
                "keepalpha",           // do not force opaque alpha
            };
            for (const char* candidate : kImplemented)
            {
                if (name == candidate)
                {
                    return {true, true};
                }
            }

            if (name == "exclude_path")
            {
                // We generate neither a Deferred nor a legacy PrePass pass,
                // so both exclusions are already satisfied.
                return {true, true};
            }

            // Value-bearing options ZEngine does implement.
            if (name == "vertex" || name == "finalcolor" || name == "alphatest")
            {
                return {true, true};
            }

            // --- known but ignored ------------------------------------------
            static const char* const kUnimplemented[] = {
                "fullforwardshadows",  // shadows in the add pass: no shadow
                                       // pipeline, and no add-pass shadows
                "softvegetation", "interpolateview", "halfasview", "approxview",
                "dualforward", "directional", "lerpdetail",
                "decal", "tessellate", "tessphong",
                "finalgbuffer", "finalprepass",
            };
            for (const char* candidate : kUnimplemented)
            {
                if (name == candidate)
                {
                    return {true, false};
                }
            }

            return {false, false};
        }

    }  // namespace

    bool SurfaceShaderDirective::HasOption(const std::string& option) const
    {
        const std::string wanted = ToLower(option);
        return std::find(options.begin(), options.end(), wanted) != options.end();
    }

    bool SurfaceShaderDirective::OptionValue(const std::string& prefix,
                                             std::string& value_out) const
    {
        const std::string wanted = ToLower(prefix);
        for (const std::string& raw : raw_options)
        {
            if (ToLower(raw).rfind(wanted, 0) != 0)
            {
                continue;
            }
            value_out = raw.substr(prefix.size());
            return !value_out.empty();
        }
        return false;
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
            else if (IsValidIdentifier(out.model_name))
            {
                // Not one of Unity's built-ins: the shader is expected to
                // supply `Lighting<Name>()`. Resolved in
                // GenerateSurfacePasses(), which is where the body is
                // available -- an unknown model is a *valid* name until then.
                out.model = SurfaceLightingModel::Custom;
            }
            else
            {
                out.model = SurfaceLightingModel::Unknown;
                out.error = "invalid surface lighting model '" + out.model_name +
                            "' (built-ins: Standard, Lambert, BlinnPhong; otherwise the "
                            "name of a Lighting<Name> function)";
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
                out.raw_options.push_back(tokens[i]);
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
        // Used wherever the question is "does the body already declare this?":
        // a `#pragma` line that merely mentions a name must not count.
        const std::string masked_body = MaskPreprocessorLines(masked);

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

        // ---- custom lighting model ----------------------------------------
        // `#pragma surface surf Simple` means "call LightingSimple()". Unity
        // accepts two shapes; the GI forms (`LightingX_GI`, and a two-argument
        // `LightingX(s, UnityGI)`) are rejected because ZEngine has no GI.
        struct CustomLighting
        {
            std::string name;
            int argument_count = 0;  // 3 = (s, lightDir, atten), 4 = + viewDir
        };
        CustomLighting custom;

        if (directive.model == SurfaceLightingModel::Custom)
        {
            custom.name = "Lighting" + directive.model_name;
            const FunctionRef ref = FindFunctionParams(masked, custom.name);
            if (!ref.found)
            {
                out.error = "lighting model '" + directive.model_name + "' requires '" +
                            custom.name + "()', which is not defined in this CGPROGRAM";
                return out;
            }
            if (ref.parameters.size() != 3 && ref.parameters.size() != 4)
            {
                out.error = "'" + custom.name + "()' takes " +
                            std::to_string(ref.parameters.size()) +
                            " parameters; ZEngine supports the two Unity forms "
                            "(SurfaceOutput s, half3 lightDir, half atten) and "
                            "(SurfaceOutput s, half3 lightDir, half3 viewDir, half atten) -- "
                            "the UnityGI form is not supported";
                return out;
            }
            const std::string first = ParameterTypeOf(ref.parameters[0]);
            if (first != signature.output_type)
            {
                out.error = "'" + custom.name + "()' takes '" + first + "', but '" +
                            directive.function + "' writes '" + signature.output_type +
                            "' -- the two must agree";
                return out;
            }
            custom.argument_count = static_cast<int>(ref.parameters.size());
        }
        else
        {
            const bool model_wants_standard = (directive.model == SurfaceLightingModel::Standard);
            if (model_wants_standard != output_is_standard)
            {
                out.error = "lighting model '" + directive.model_name + "' expects inout " +
                            (model_wants_standard ? "SurfaceOutputStandard" : "SurfaceOutput") +
                            ", but '" + directive.function + "' declares inout " +
                            signature.output_type;
                return out;
            }
        }

        // ---- `vertex:` / `finalcolor:` ------------------------------------
        std::string vertex_modifier;
        if (directive.OptionValue("vertex:", vertex_modifier))
        {
            if (!IsValidIdentifier(vertex_modifier))
            {
                out.error = "'vertex:" + vertex_modifier + "' is not a function name";
                return out;
            }
            if (!FindFunctionParams(masked, vertex_modifier).found)
            {
                out.error = "'vertex:" + vertex_modifier + "' names a function that is not "
                            "defined in this CGPROGRAM";
                return out;
            }
        }
        else
        {
            vertex_modifier.clear();
        }

        std::string final_color;
        if (directive.OptionValue("finalcolor:", final_color))
        {
            if (!IsValidIdentifier(final_color))
            {
                out.error = "'finalcolor:" + final_color + "' is not a function name";
                return out;
            }
            if (!FindFunctionParams(masked, final_color).found)
            {
                out.error = "'finalcolor:" + final_color + "' names a function that is not "
                            "defined in this CGPROGRAM";
                return out;
            }
        }
        else
        {
            final_color.clear();
        }

        std::string alpha_test;
        if (directive.OptionValue("alphatest:", alpha_test) && !IsValidIdentifier(alpha_test))
        {
            out.error = "'alphatest:" + alpha_test + "' is not a variable name";
            return out;
        }

        // The UnityCG shim's instancing macros compile -- but into a
        // one-instance form, so every instance would render identically. That
        // is a silent wrong result, so it has to be said out loud here rather
        // than being discoverable only by reading the shim.
        //
        // UNITY_INSTANCING_BUFFER_START alone is NOT enough to warn: Unity's
        // own "Standard Surface Shader" template carries an empty Props block,
        // so warning on it would fire for every template-derived shader. Only
        // a shader that actually declares or reads an instanced property is
        // relying on instancing.
        static const char* const kInstancingMacros[] = {
            "UNITY_DEFINE_INSTANCED_PROP",
            "UNITY_ACCESS_INSTANCED_PROP",
            "UNITY_SETUP_INSTANCE_ID",
            "UNITY_TRANSFER_INSTANCE_ID",
        };
        for (const char* macro : kInstancingMacros)
        {
            if (FindWholeWord(masked, macro, 0) != kNpos)
            {
                out.warnings.push_back(std::string("'") + macro +
                                       "': ZEngine has no instancing path yet, so every "
                                       "instance renders identically");
                break;
            }
        }

        // ---- options: warn instead of silently ignoring -------------------
        for (size_t i = 0; i < directive.options.size(); ++i)
        {
            const std::string& option = directive.options[i];
            // Report the option AS WRITTEN: `options` is lower-cased for
            // matching, so a warning quoting it would not match what the
            // author sees in the shader.
            const std::string& as_written =
                (i < directive.raw_options.size()) ? directive.raw_options[i] : option;

            const OptionInfo info = ClassifyOption(option);
            if (!info.known)
            {
                out.warnings.push_back("unknown surface option '" + as_written + "'; ignored");
            }
            else if (!info.implemented)
            {
                out.warnings.push_back("surface option '" + as_written +
                                       "' is recognised but not implemented yet; ignored");
            }
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
                !BodyDeclaresName(masked_body, st))
            {
                st_uniforms.push_back(st);
            }
        }

        bool has_view_dir_member = false;
        bool need_screen_pos = false;
        for (const InputMember& member : members)
        {
            if (member.kind == InputMemberKind::ViewDir)
            {
                has_view_dir_member = true;
            }
            if (member.kind == InputMemberKind::ScreenPos)
            {
                need_screen_pos = true;
            }
            if (member.kind == InputMemberKind::Unsupported)
            {
                out.warnings.push_back("Input member '" + member.name +
                                       "' is not filled: ZEngine does not know how to "
                                       "generate it yet (it will read as zero)");
            }
        }

        // `o.Normal` is a TANGENT-space normal, so honouring it costs a
        // tangent/bitangent pair of varyings. Only pay for it when the author
        // actually writes one -- an unwritten `o.Normal` is (0,0,1), which
        // leaves the vertex normal alone.
        //
        // The test is textual (`.Normal` anywhere in the body) because the
        // write may be behind a helper or a macro; over-detecting only costs
        // one interpolator, under-detecting silently drops normal mapping.
        const bool need_tangent = masked.find(".Normal") != kNpos;

        const bool need_view_dir =
            has_view_dir_member ||
            directive.model == SurfaceLightingModel::Standard ||
            directive.model == SurfaceLightingModel::BlinnPhong ||
            custom.argument_count == 4;

        // ---- varying layout -------------------------------------------------
        // worldPos / worldNormal are unconditional: the lighting math needs
        // them even when `Input` does not ask for them.
        struct VaryingField
        {
            std::string type;
            std::string name;
            int uv_channel = -1;  // 0 = TEXCOORD0 source, 1 = TEXCOORD1 source
            std::string texture;  // uv members only
        };

        std::vector<VaryingField> varyings;
        int texcoord_index = 0;

        for (const InputMember& member : members)
        {
            if (member.kind == InputMemberKind::Uv || member.kind == InputMemberKind::Uv2)
            {
                VaryingField field;
                field.type = "float2";
                field.name = member.name;
                field.uv_channel = (member.kind == InputMemberKind::Uv) ? 0 : 1;
                field.texture = member.texture;
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

        if (need_tangent)
        {
            VaryingField world_tangent;
            world_tangent.type = "float4";  // xyz = world tangent, w = handedness
            world_tangent.name = "zWorldTangent";
            varyings.push_back(world_tangent);
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

        // `alphatest:_Cutoff` reads a variable the shader may never declare
        // (Unity's own templates declare it because a Properties block implies
        // it, which ZEngine's pass generator does not know about).
        if (!alpha_test.empty() && !BodyDeclaresName(masked_body, alpha_test))
        {
            preamble += "float " + alpha_test + ";\n";
        }

        // ---- build the common preamble --------------------------------------
        std::ostringstream code;
        code << "\n";
        code << "// ------------------------------------------------------------------\n";
        code << "// Generated by ZEngine's surface-shader desugaring for\n";
        code << "//   #pragma surface " << directive.function << " " << directive.model_name;
        for (const std::string& option : directive.raw_options)
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
            code << "    " << field.type << " " << field.name << " : TEXCOORD"
                 << texcoord_index << ";\n";
            ++texcoord_index;
        }
        code << "};\n";

        // ---- vertex shader ---------------------------------------------------
        //
        // With `vertex:<fn>` the generated vertex shader works on an
        // `appdata_full` copy instead of its own input struct, because that is
        // the type Unity hands to a vertex modifier -- the author's function
        // is written against `appdata_full`'s members.
        const bool use_appdata = !vertex_modifier.empty();
        const std::string vsrc = use_appdata ? "zApp" : "v";
        // appdata_full.texcoord is a float4 (Unity); ZSurfaceVertexInput's is
        // a float2, so the uv expression has to differ between the two.
        const std::string uv0 = use_appdata ? "zApp.texcoord.xy" : "v.texcoord";
        const std::string uv1 = use_appdata ? "zApp.texcoord1.xy" : "v.texcoord1";

        code << "\n";
        code << "ZSurfaceVarying zSurf_vert (ZSurfaceVertexInput v)\n";
        code << "{\n";
        code << "    ZSurfaceVarying o;\n";
        if (use_appdata)
        {
            code << "    appdata_full zApp;\n";
            code << "    zApp.vertex    = v.vertex;\n";
            code << "    zApp.tangent   = v.tangent;\n";
            code << "    zApp.normal    = v.normal;\n";
            code << "    zApp.texcoord  = float4(v.texcoord, 0.0, 0.0);\n";
            code << "    zApp.texcoord1 = float4(v.texcoord1, 0.0, 0.0);\n";
            code << "    zApp.texcoord2 = float4(0.0, 0.0, 0.0, 0.0);\n";
            code << "    zApp.texcoord3 = float4(0.0, 0.0, 0.0, 0.0);\n";
            code << "    zApp.color     = float4(1.0, 1.0, 1.0, 1.0);\n";
            code << "    " << vertex_modifier << " (zApp);\n";
        }
        code << "    o.zClipPos = UnityObjectToClipPos(" << vsrc << ".vertex);\n";
        code << "    const float4 zWorldPos4 = mul(unity_ObjectToWorld, " << vsrc << ".vertex);\n";
        code << "    o.zWorldPos = zWorldPos4.xyz;\n";
        code << "    o.zWorldNormal = UnityObjectToWorldDir(" << vsrc << ".normal);\n";
        if (need_tangent)
        {
            code << "    o.zWorldTangent = float4(UnityObjectToWorldDir(" << vsrc
                 << ".tangent.xyz), " << vsrc << ".tangent.w);\n";
        }
        for (const VaryingField& field : varyings)
        {
            if (field.uv_channel == 0 || field.uv_channel == 1)
            {
                const std::string& source_uv = (field.uv_channel == 0) ? uv0 : uv1;
                code << "    o." << field.name << " = " << source_uv << " * " << field.texture
                     << "_ST.xy + " << field.texture << "_ST.zw;\n";
            }
            else if (field.name == "zScreenPos")
            {
                code << "    o.zScreenPos = o.zClipPos;\n";
            }
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
            if (!alpha_test.empty())
            {
                // Unity emits this right after `surf`, in EVERY pass -- so a
                // cut-out shader discards in the shadow pass too.
                target << "    clip(o.Alpha - " << alpha_test << ");\n";
            }
        };

        // The shading frame: the world-space normal the lighting math runs on.
        // Shared by the forward passes and the GBuffer pass, so a normal map
        // cannot end up applying to one and not the other.
        auto emit_shading_frame = [&](std::ostringstream& target) {
            target << "\n";
            target << "    const float3 zVertexNormal = normalize(IN.zWorldNormal);\n";
            target << "    float3 zN = zVertexNormal;\n";
            if (need_tangent)
            {
                target << "\n";
                target << "    // o.Normal is TANGENT space (Unity's convention); (0,0,1) is\n";
                target << "    // 'unperturbed'. Build the tangent frame and rotate into\n";
                target << "    // world space so a normal map actually does something.\n";
                target << "    float3 zT = IN.zWorldTangent.xyz;\n";
                target << "    float zTSign = IN.zWorldTangent.w;\n";
                target << "    if (dot(zT, zT) < 1e-8)\n";
                target << "    {\n";
                target << "        // A mesh without tangents (or with degenerate ones) would\n";
                target << "        // make normalize() NaN and poison every pixel. Pick any\n";
                target << "        // vector that is not parallel to the normal instead, so\n";
                target << "        // o.Normal still tilts the surface.\n";
                target << "        zT = (abs(zVertexNormal.y) < 0.999)\n";
                target << "            ? normalize(cross(float3(0.0, 1.0, 0.0), zVertexNormal))\n";
                target << "            : normalize(cross(float3(1.0, 0.0, 0.0), zVertexNormal));\n";
                target << "        zTSign = 1.0;\n";
                target << "    }\n";
                target << "    zT = normalize(zT);\n";
                target << "    const float3 zB = cross(zVertexNormal, zT) * ((zTSign < 0.0) ? -1.0 : 1.0);\n";
                target << "    zN = normalize(o.Normal.x * zT + o.Normal.y * zB + o.Normal.z * zVertexNormal);\n";
            }
        };

        // ---- lighting -------------------------------------------------------
        //
        // One generator, parameterised by which light it is lit by:
        // ForwardBase uses _WorldSpaceLightPos0, ForwardAdd uses
        // _WorldSpaceLightPos1.
        auto emit_lit_fragment = [&](std::ostringstream& target,
                                     const std::string& fragment_name,
                                     const std::string& light_color,
                                     const std::string& light_pos) {
            target << "\n";
            target << "float4 " << fragment_name << " (ZSurfaceVarying IN) : SV_Target\n";
            target << "{\n";
            emit_surf_setup(target);

            const bool is_custom = (custom.argument_count != 0);

            emit_shading_frame(target);

            target << "\n";
            target << "    // Unity's UnityWorldSpaceLightDir(): _WorldSpaceLightPos0.w is 0\n";
            target << "    // for a directional light (so .xyz already IS the direction) and\n";
            target << "    // 1 for a point light (so the direction is pos - worldPos).\n";
            target << "    const float3 zL = normalize(" << light_pos
                   << ".xyz - IN.zWorldPos * " << light_pos << ".w);\n";
            if (need_view_dir)
            {
                target << "    const float3 zV = normalize(_WorldSpaceCameraPos - IN.zWorldPos);\n";
            }
            target << "    const float zNdL = saturate(dot(zN, zL));\n";
            target << "\n";

            if (is_custom)
            {
                target << "    // " << custom.name << "() is the author's own lighting\n";
                target << "    // function. `atten` is 1.0: ZEngine has no shadow or\n";
                target << "    // per-light attenuation pipeline wired to ShaderLab yet.\n";
                if (custom.argument_count == 3)
                {
                    target << "    const float4 zLit = " << custom.name
                           << " (o, zL, 1.0);\n";
                }
                else
                {
                    target << "    const float4 zLit = " << custom.name
                           << " (o, zL, zV, 1.0);\n";
                }
                target << "    float3 zColor = zLit.rgb;\n";
                target << "    zColor += o.Emission;\n";
                target << "    float4 zColor4 = float4(zColor, "
                       << (directive.HasOption("keepalpha") ? "zLit.a" : "o.Alpha") << ");\n";
            }
            else if (directive.model == SurfaceLightingModel::Standard)
            {
                // A deliberately simplified metallic/roughness approximation.
                // It is NOT Unity's Standard BRDF -- ZEngine has no GI, no
                // light list and no BRDF data yet. What matters structurally is
                // that every field the author wrote is read.
                target << "    const float3 zH = normalize(zL + zV);\n";
                target << "    const float zNdH = saturate(dot(zN, zH));\n";
                target << "    const float zSpecPower = exp2(10.0 * o.Smoothness + 1.0);\n";
                target << "    const float3 zSpecColor = lerp(float3(0.04, 0.04, 0.04), o.Albedo, o.Metallic);\n";
                target << "    const float3 zDiffuseColor = o.Albedo * (1.0 - o.Metallic);\n";
                target << "    float3 zColor = zDiffuseColor * " << light_color << ".rgb * zNdL;\n";
                target << "    zColor += zSpecColor * " << light_color
                       << ".rgb * pow(zNdH, zSpecPower) * zNdL;\n";
                target << "    zColor *= o.Occlusion;\n";
                target << "    zColor += o.Emission;\n";
                target << "    float4 zColor4 = float4(zColor, o.Alpha);\n";
            }
            else if (directive.model == SurfaceLightingModel::BlinnPhong)
            {
                target << "    const float3 zH = normalize(zL + zV);\n";
                target << "    const float zNdH = saturate(dot(zN, zH));\n";
                target << "    float3 zColor = o.Albedo * " << light_color << ".rgb * zNdL;\n";
                target << "    zColor += " << light_color
                       << ".rgb * o.Specular * pow(zNdH, o.Gloss * 128.0 + 1.0) * zNdL;\n";
                target << "    zColor += o.Emission;\n";
                target << "    float4 zColor4 = float4(zColor, o.Alpha);\n";
            }
            else  // Lambert
            {
                target << "    float3 zColor = o.Albedo * " << light_color << ".rgb * zNdL;\n";
                target << "    zColor += o.Emission;\n";
                target << "    float4 zColor4 = float4(zColor, o.Alpha);\n";
            }

            // Unity's UNITY_OPAQUE_ALPHA: without `alpha` / `keepalpha` a
            // surface shader is opaque, and an `o.Alpha` the author wrote is
            // ignored on purpose -- a fade written into an opaque queue would
            // be a silent sorting bug, not a feature.
            if (!directive.HasOption("alpha") && !directive.HasOption("keepalpha"))
            {
                target << "    zColor4.a = 1.0;\n";
            }

            if (!final_color.empty())
            {
                target << "    " << final_color << " (zSurfIN, o, zColor4);\n";
            }

            target << "    return zColor4;\n";
            target << "}\n";
        };

        // `alpha` turns the forward passes transparent.
        SurfaceRenderStateOverride forward_state;
        if (directive.HasOption("alpha"))
        {
            forward_state.has_blend = true;
            forward_state.blend_src = SurfaceBlendFactor::SrcAlpha;
            forward_state.blend_dst = SurfaceBlendFactor::OneMinusSrcAlpha;
            forward_state.zwrite = true;
        }

        // ---- ForwardBase ----------------------------------------------------
        {
            std::ostringstream pass_code;
            pass_code << code.str();
            emit_lit_fragment(pass_code, "zSurf_frag", "_LightColor0", "_WorldSpaceLightPos0");

            SurfacePass pass;
            pass.name = "ForwardBase";
            pass.light_mode = "ForwardBase";
            pass.vertex_entry = "zSurf_vert";
            pass.fragment_entry = "zSurf_frag";
            pass.render_state = forward_state;
            pass.source_code = preamble + program_source + pass_code.str();
            out.passes.push_back(std::move(pass));
        }

        // ---- ForwardAdd -----------------------------------------------------
        //
        // Unity always emits this pass (unless `noforwardadd`): one additive
        // draw per additional light. ZEngine has no per-object light list, so
        // it is lit by a single second light, `_LightColor1` /
        // `_WorldSpaceLightPos1` -- unbound, i.e. black, i.e. a no-op until
        // the engine learns to fill them. Generating it keeps the pass list
        // structurally identical to Unity's and gives a real seam to plug a
        // light list into; `noforwardadd` skips the draw entirely.
        if (!directive.HasOption("noforwardadd"))
        {
            SurfaceRenderStateOverride add_state = forward_state;
            add_state.has_blend = true;
            add_state.blend_src = SurfaceBlendFactor::One;
            add_state.blend_dst = SurfaceBlendFactor::One;
            add_state.has_zwrite = true;
            add_state.zwrite = false;

            std::ostringstream pass_code;
            pass_code << code.str();
            pass_code << "\n";
            pass_code << "// Additional per-pixel lights. ZEngine exposes one light, so this\n";
            pass_code << "// pass reads _LightColor1/_WorldSpaceLightPos1 -- unbound (black)\n";
            pass_code << "// until a real light list exists. Additive blending makes an\n";
            pass_code << "// unlit second light a genuine no-op rather than a darkening.\n";
            emit_lit_fragment(pass_code, "zSurf_addFrag", "_LightColor1", "_WorldSpaceLightPos1");

            SurfacePass pass;
            pass.name = "ForwardAdd";
            pass.light_mode = "ForwardAdd";
            pass.vertex_entry = "zSurf_vert";
            pass.fragment_entry = "zSurf_addFrag";
            pass.render_state = add_state;
            pass.source_code = preamble + program_source + pass_code.str();
            out.passes.push_back(std::move(pass));
        }

        // ---- shadow caster --------------------------------------------------
        //
        // Unity relies on the shader's Fallback for the shadow caster and only
        // generates one itself when `addshadow` is given (which is what you
        // need when a vertex modifier has to apply to the shadow too).
        //
        // ZEngine does not resolve Fallback, so "use the fallback's caster"
        // means "has no caster at all" -- and ZEngine DOES have a shadow
        // pipeline that looks the pass up by LightMode:
        // DirectionalLightPass.cpp / PointLightPass.cpp both call
        // FindShaderPassByLightMode(material, "ShadowCaster"). So the pass is
        // generated by default and `noshadow` suppresses it; `addshadow`
        // stays accepted (and is now simply redundant).
        if (!directive.HasOption("noshadow"))
        {
            std::ostringstream pass_code;
            pass_code << code.str();
            pass_code << "\n";
            pass_code << "// Shadow caster: ZEngine has no shadow pipeline wired to ShaderLab\n";
            pass_code << "// yet, so this writes depth only. `surf` is still called so that a\n";
            pass_code << "// cutout shader's alphatest clip() applies here too.\n";
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

        // ---- GBuffer (Unity calls this the Deferred pass) ---------------------
        //
        // Unity generates this unless `exclude_path:deferred`. ZEngine can
        // actually RUN it: RenderResourceBase picks "GBuffer" as the material's
        // primary pass, RenderScene routes a material that HAS a GBuffer pass
        // into the deferred bucket, and MainCameraRp1Pass builds a pipeline
        // from that pass's own vertex/fragment entry points.
        //
        // Two hard constraints, both discovered by reading the consumer rather
        // than Unity's docs:
        //
        //   * EXACTLY THREE render targets. CreateRuntimeMeshPipeline(..., 3)
        //     binds gbuffer_a/b/c and nothing else -- RenderPass.h defines no
        //     fourth attachment. A 4-target output cannot form a PSO and would
        //     silently fall back to the built-in mesh_gbuffer shader, which
        //     throws away everything the author's surf() did.
        //
        //   * The layout is normal / material / albedo -- the same one
        //     engine/shader/example_standard.shader now writes. deferred_lighting
        //     .frag.hlsl decodes gbuffer_a with DecodeNormal() and MainCamera
        //     Rp1Pass binds gbuffer_a as "gbuffer_normal" and gbuffer_c as
        //     "gbuffer_albedo", matching hlsl/rp1/mesh_gbuffer.frag.hlsl.
        //
        // There is no emissive slot in this GBuffer, so a shader that writes
        // o.Emission does NOT get a GBuffer pass: having one routes the
        // material away from the forward pass (RenderScene.cpp), where
        // emission does work, and would silently drop it.
        const bool writes_emission = masked.find(".Emission") != kNpos;
        const bool wants_deferred = !directive.HasOption("alpha") && !writes_emission;

        if (wants_deferred && !directive.HasOption("exclude_path:deferred"))
        {
            std::ostringstream pass_code;
            pass_code << code.str();
            pass_code << "\n";
            pass_code << "// Unity's DEFERRED pass, in ZEngine's GBuffer layout:\n";
            pass_code << "//   SV_Target0 = packed world normal\n";
            pass_code << "//   SV_Target1 = metallic, specular, roughness, shading model\n";
            pass_code << "//   SV_Target2 = albedo\n";
            pass_code << "struct ZSurfaceGBuffer\n";
            pass_code << "{\n";
            pass_code << "    float4 zGBuffer0 : SV_Target0;\n";
            pass_code << "    float4 zGBuffer1 : SV_Target1;\n";
            pass_code << "    float4 zGBuffer2 : SV_Target2;\n";
            pass_code << "};\n";
            pass_code << "\n";
            pass_code << "void zSurf_gbufferFrag (ZSurfaceVarying IN, out ZSurfaceGBuffer zOut)\n";
            pass_code << "{\n";
            emit_surf_setup(pass_code);
            emit_shading_frame(pass_code);
            pass_code << "\n";
            if (output_is_standard)
            {
                pass_code << "    const float zMetallic = saturate(o.Metallic);\n";
                pass_code << "    const float zSpecular = 0.5;\n";
                pass_code << "    const float zRoughness = saturate(1.0 - o.Smoothness);\n";
            }
            else
            {
                pass_code << "    const float zMetallic = 0.0;\n";
                pass_code << "    const float zSpecular = saturate(o.Specular);\n";
                pass_code << "    const float zRoughness = saturate(1.0 - o.Gloss);\n";
            }
            pass_code << "\n";
            pass_code << "    zOut.zGBuffer0 = float4(zN * 0.5 + 0.5, 1.0);\n";
            // SHADINGMODELID_DEFAULT_LIT == 1, packed the way
            // DecodeShadingModelId() unpacks it (round(a * 255)).
            pass_code << "    zOut.zGBuffer1 = float4(zMetallic, zSpecular, zRoughness, 1.0 / 255.0);\n";
            pass_code << "    zOut.zGBuffer2 = float4(o.Albedo, 1.0);\n";
            pass_code << "}\n";

            SurfacePass pass;
            pass.name = "GBuffer";
            pass.light_mode = "GBuffer";
            pass.vertex_entry = "zSurf_vert";
            pass.fragment_entry = "zSurf_gbufferFrag";
            pass.source_code = preamble + program_source + pass_code.str();
            out.passes.push_back(std::move(pass));
        }
        else if (writes_emission && !directive.HasOption("exclude_path:deferred"))
        {
            out.warnings.push_back("no GBuffer (deferred) pass is generated: ZEngine's GBuffer "
                                   "has three render targets and no emissive slot, so "
                                   "o.Emission would be lost. The forward passes still carry "
                                   "it.");
        }

        out.ok = true;
        return out;
    }

}  // namespace ZEngine::ShaderLab
