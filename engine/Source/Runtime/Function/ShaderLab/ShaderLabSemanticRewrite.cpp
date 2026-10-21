#include "Runtime/Function/ShaderLab/ShaderLabSemanticRewrite.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace ZEngine::ShaderLab
{

    namespace
    {

        bool IsIdentChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        std::string ToUpperAscii(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            return s;
        }

        // ---------------------------------------------------------------------
        // A copy of `src` with every comment and string/char literal replaced by
        // spaces. Length and newline positions are preserved, so an index into
        // the masked string is also an index into the original.
        //
        // Without this, `float4 vertexColor : COLOR;` sitting inside a
        // commented-out experiment -- or the word "frag" inside a string --
        // would look exactly like the declaration we are hunting for.
        // ---------------------------------------------------------------------
        std::string MaskNonCode(const std::string& src)
        {
            std::string out(src.size(), ' ');

            auto blank = [&out, &src](size_t i) { out[i] = (src[i] == '\n') ? '\n' : ' '; };

            size_t i = 0;
            while (i < src.size())
            {
                if (src[i] == '\n')
                {
                    out[i] = '\n';
                    ++i;
                    continue;
                }

                // Line comment.
                if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
                {
                    while (i < src.size() && src[i] != '\n')
                    {
                        blank(i);
                        ++i;
                    }
                    continue;
                }

                // Block comment.
                if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*')
                {
                    blank(i);
                    ++i;
                    blank(i);
                    ++i;
                    while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/'))
                    {
                        blank(i);
                        ++i;
                    }
                    if (i < src.size())
                    {
                        blank(i);
                        ++i;
                    }
                    if (i < src.size())
                    {
                        blank(i);
                        ++i;
                    }
                    continue;
                }

                // String / char literal.
                if (src[i] == '"' || src[i] == '\'')
                {
                    const char quote = src[i];
                    blank(i);
                    ++i;
                    while (i < src.size() && src[i] != quote)
                    {
                        if (src[i] == '\\' && i + 1 < src.size())
                        {
                            blank(i);
                            ++i;
                            blank(i);
                            ++i;
                            continue;
                        }
                        blank(i);
                        ++i;
                    }
                    if (i < src.size())
                    {
                        blank(i);  // closing quote
                        ++i;
                    }
                    continue;
                }

                out[i] = src[i];
                ++i;
            }

            return out;
        }

        size_t SkipBlanks(const std::string& s, size_t i)
        {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0)
            {
                ++i;
            }
            return i;
        }

        // Index just past the `(` at `open`, i.e. the index of its matching ')'.
        // Returns npos when the parens are unbalanced (malformed source: we
        // bail out rather than guess).
        size_t MatchingParen(const std::string& s, size_t open)
        {
            int depth = 0;
            for (size_t i = open; i < s.size(); ++i)
            {
                if (s[i] == '(')
                {
                    ++depth;
                }
                else if (s[i] == ')')
                {
                    --depth;
                    if (depth == 0)
                    {
                        return i;
                    }
                }
            }
            return std::string::npos;
        }

        // COLOR -> SV_Target, COLOR0..COLOR7 -> SV_Target0..SV_Target7.
        // Anything else (including every SV_* name) maps to the empty string
        // and is left alone.
        //
        // ps_6_0 caps render targets at 8, so COLOR8+ is deliberately NOT
        // mapped: there is no correct SV_Target to substitute, and silently
        // picking one would hide a real authoring mistake.
        std::string MapLegacyColorSemantic(const std::string& semantic)
        {
            const std::string upper = ToUpperAscii(semantic);
            if (upper == "COLOR")
            {
                return "SV_Target";
            }
            if (upper.size() == 6 && upper.compare(0, 5, "COLOR") == 0 && upper[5] >= '0' &&
                upper[5] <= '7')
            {
                return "SV_Target" + std::string(1, upper[5]);
            }
            return {};
        }

        struct Edit
        {
            size_t begin = 0;
            size_t end = 0;
            std::string replacement;
        };

        // True when `pos` is preceded by `.` or `::` -- i.e. the identifier is
        // a member (`obj.frag(...)` / `Ns::frag(...)`), not a free function.
        bool IsQualifiedName(const std::string& masked, size_t pos)
        {
            size_t i = pos;
            while (i > 0 && std::isspace(static_cast<unsigned char>(masked[i - 1])) != 0)
            {
                --i;
            }
            if (i > 0 && masked[i - 1] == '.')
            {
                return true;
            }
            if (i >= 2 && masked[i - 1] == ':' && masked[i - 2] == ':')
            {
                return true;
            }
            return false;
        }

    }  // namespace

    SemanticRewriteResult RewriteLegacyOutputSemantics(const std::string& hlsl,
                                                      const std::string& entry_point,
                                                      ShaderStage stage)
    {
        SemanticRewriteResult result;
        result.code = hlsl;

        // `COLOR` is a legal INPUT semantic on a vertex shader, so a vertex
        // stage is never touched -- rewriting there would break shaders that
        // are already correct.
        if (entry_point.empty() || stage != ShaderStage::Fragment)
        {
            return result;
        }

        const std::string masked = MaskNonCode(hlsl);

        std::vector<Edit> edits;
        const size_t name_len = entry_point.size();

        for (size_t pos = masked.find(entry_point, 0); pos != std::string::npos;
             pos = masked.find(entry_point, pos + name_len))
        {
            // Whole identifier only: `myfrag` / `fragColor` must not match.
            if (pos > 0 && IsIdentChar(masked[pos - 1]))
            {
                continue;
            }
            const size_t name_end = pos + name_len;
            if (name_end < masked.size() && IsIdentChar(masked[name_end]))
            {
                continue;
            }
            if (IsQualifiedName(masked, pos))
            {
                continue;
            }

            // Must be a function declarator: the next code character is '('.
            const size_t paren_open = SkipBlanks(masked, name_end);
            if (paren_open >= masked.size() || masked[paren_open] != '(')
            {
                continue;
            }

            const size_t paren_close = MatchingParen(masked, paren_open);
            if (paren_close == std::string::npos)
            {
                break;  // unbalanced parens -- stop scanning, source is broken
            }

            // The output semantic lives between ')' and the '{' of the body
            // (or the ';' of a forward declaration). Parameter semantics are
            // INSIDE the parens and are therefore never seen here -- that is
            // the whole point of scanning this window instead of the file.
            size_t scan = paren_close + 1;
            size_t window_end = std::string::npos;
            while (scan < masked.size())
            {
                if (masked[scan] == '{' || masked[scan] == ';')
                {
                    window_end = scan;
                    break;
                }
                ++scan;
            }
            if (window_end == std::string::npos)
            {
                continue;
            }

            const size_t colon = masked.find(':', paren_close + 1);
            if (colon == std::string::npos || colon >= window_end)
            {
                continue;  // no output semantic (void, or a struct return)
            }

            const size_t sem_begin = SkipBlanks(masked, colon + 1);
            size_t sem_end = sem_begin;
            while (sem_end < masked.size() && IsIdentChar(masked[sem_end]))
            {
                ++sem_end;
            }
            if (sem_end == sem_begin)
            {
                continue;
            }

            const std::string semantic = masked.substr(sem_begin, sem_end - sem_begin);
            const std::string replacement = MapLegacyColorSemantic(semantic);
            if (replacement.empty())
            {
                continue;  // already SV_*, or something we must not guess at
            }

            edits.push_back({sem_begin, sem_end, replacement});
        }

        // Apply back-to-front so earlier offsets stay valid.
        std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
            return a.begin > b.begin;
        });
        for (const Edit& e : edits)
        {
            result.code.replace(e.begin, e.end - e.begin, e.replacement);
        }

        result.rewrites = static_cast<int>(edits.size());
        return result;
    }

}  // namespace ZEngine::ShaderLab
