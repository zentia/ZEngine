#include "ShaderLabIncludeProcessor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace ZEngine::ShaderLab
{

    namespace
    {

        // Belt-and-braces. The cycle check below already stops runaway
        // recursion; this caps the damage if a pathological include graph
        // ever slips past it, and turns a stack overflow into an error.
        constexpr int kMaxIncludeDepth = 64;

        // `word` must appear at `pos` and must not be followed by an
        // identifier character -- otherwise "#pragma onceFoo" would match
        // "once" and "#includefoo" would match "include".
        bool MatchWord(const std::string& line, size_t pos, const char* word)
        {
            const size_t n = std::strlen(word);
            if (line.compare(pos, n, word) != 0)
            {
                return false;
            }
            const size_t after = pos + n;
            if (after >= line.size())
            {
                return true;
            }
            const unsigned char c = static_cast<unsigned char>(line[after]);
            return !std::isalnum(c) && c != '_';
        }

        // Index of the first non-blank character, or npos.
        size_t FirstNonBlank(const std::string& line)
        {
            return line.find_first_not_of(" \t\r");
        }

        // A line whose first non-blank characters begin a comment is never a
        // preprocessor directive, even if it contains a '#'.
        bool IsCommentLine(const std::string& line)
        {
            const size_t i = FirstNonBlank(line);
            if (i == std::string::npos || line[i] != '/')
            {
                return false;
            }
            const size_t next = i + 1;
            return next < line.size() && (line[next] == '/' || line[next] == '*');
        }

        // Parses `#include "x"` / `#include <x>`. `out_angle` distinguishes
        // the two forms because they search different directories.
        bool ParseInclude(const std::string& line, std::string& out_target, bool& out_angle)
        {
            if (IsCommentLine(line))
            {
                return false;
            }

            size_t i = FirstNonBlank(line);
            if (i == std::string::npos || line[i] != '#')
            {
                return false;
            }

            i = line.find_first_not_of(" \t", i + 1);
            if (i == std::string::npos || !MatchWord(line, i, "include"))
            {
                return false;
            }

            i = line.find_first_not_of(" \t", i + std::strlen("include"));
            if (i == std::string::npos)
            {
                return false;
            }

            const char open = line[i];
            const char close = (open == '"') ? '"' : (open == '<' ? '>' : '\0');
            if (close == '\0')
            {
                return false;
            }

            const size_t end = line.find(close, i + 1);
            if (end == std::string::npos)
            {
                return false;
            }

            out_target = line.substr(i + 1, end - i - 1);
            out_angle = (open == '<');
            return !out_target.empty();
        }

        bool IsPragmaOnce(const std::string& line)
        {
            if (IsCommentLine(line))
            {
                return false;
            }

            size_t i = FirstNonBlank(line);
            if (i == std::string::npos || line[i] != '#')
            {
                return false;
            }

            i = line.find_first_not_of(" \t", i + 1);
            if (i == std::string::npos || !MatchWord(line, i, "pragma"))
            {
                return false;
            }

            i = line.find_first_not_of(" \t", i + std::strlen("pragma"));
            return i != std::string::npos && MatchWord(line, i, "once");
        }

        // `#pragma once` conventionally sits at the top, but nothing mandates
        // that, so scan the whole file.
        bool DeclaresPragmaOnce(const std::string& text)
        {
            size_t pos = 0;
            while (pos < text.size())
            {
                const size_t nl = text.find('\n', pos);
                const size_t end = (nl == std::string::npos) ? text.size() : nl;
                if (IsPragmaOnce(text.substr(pos, end - pos)))
                {
                    return true;
                }
                if (nl == std::string::npos)
                {
                    break;
                }
                pos = nl + 1;
            }
            return false;
        }

        std::string ReadWholeFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::in | std::ios::binary);
            if (!file)
            {
                return {};
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        class IncludeExpander
        {
        public:
            explicit IncludeExpander(std::vector<std::filesystem::path> roots)
                : m_Roots(std::move(roots))
            {
            }

            bool Expand(const std::string& text,
                        const std::filesystem::path& current_dir,
                        int depth,
                        std::string& out)
            {
                if (depth > kMaxIncludeDepth)
                {
                    m_Error = "include nesting deeper than " + std::to_string(kMaxIncludeDepth) +
                              " levels";
                    return false;
                }

                int line_no = 0;
                size_t pos = 0;
                while (pos < text.size())
                {
                    const size_t nl = text.find('\n', pos);
                    const size_t line_end = (nl == std::string::npos) ? text.size() : nl;
                    const std::string line = text.substr(pos, line_end - pos);
                    const bool last_line = (nl == std::string::npos);
                    ++line_no;

                    std::string target;
                    bool angle = false;

                    if (IsPragmaOnce(line))
                    {
                        // Already accounted for by m_Once; emitting it would
                        // be a no-op directive in a fully-inlined source.
                    }
                    else if (ParseInclude(line, target, angle))
                    {
                        if (!ExpandOne(target, angle, current_dir, line_no, depth, out))
                        {
                            return false;
                        }
                    }
                    else
                    {
                        out += line;
                    }

                    if (last_line)
                    {
                        break;
                    }
                    out += '\n';
                    pos = nl + 1;
                }

                return true;
            }

            const std::string& Error() const { return m_Error; }
            const std::vector<std::filesystem::path>& IncludedFiles() const { return m_Included; }

        private:
            bool ExpandOne(const std::string& target,
                           bool angle,
                           const std::filesystem::path& current_dir,
                           int line_no,
                           int depth,
                           std::string& out)
            {
                std::filesystem::path resolved;
                std::string content;
                if (!Locate(target, angle, current_dir, resolved, content))
                {
                    m_Error = Where(line_no) + ": #include " + Quoted(target, angle) +
                              " could not be resolved; searched: " + Searched(target, angle, current_dir);
                    return false;
                }

                std::error_code ec;
                const std::filesystem::path canon = std::filesystem::weakly_canonical(resolved, ec);
                const std::filesystem::path key = ec ? resolved : canon;

                // `#pragma once` is checked before the cycle check on purpose:
                // a file that guards itself and includes itself is legal and
                // must be skipped, not reported as a cycle.
                if (DeclaresPragmaOnce(content))
                {
                    if (m_Once.count(key) != 0)
                    {
                        return true;  // second and later inclusions emit nothing
                    }
                    m_Once.insert(key);
                }

                if (std::find(m_Stack.begin(), m_Stack.end(), key) != m_Stack.end())
                {
                    m_Error = Where(line_no) + ": cyclic #include of " +
                              resolved.generic_string();
                    return false;
                }

                if (std::find(m_Included.begin(), m_Included.end(), key) == m_Included.end())
                {
                    m_Included.push_back(key);
                }

                m_Stack.push_back(key);
                const bool ok = Expand(content, resolved.parent_path(), depth + 1, out);
                m_Stack.pop_back();
                return ok;
            }

            bool Locate(const std::string& target,
                        bool angle,
                        const std::filesystem::path& current_dir,
                        std::filesystem::path& out_path,
                        std::string& out_content)
            {
                std::vector<std::filesystem::path> candidates;
                const std::filesystem::path as_written(target);

                if (as_written.is_absolute())
                {
                    candidates.push_back(as_written);
                }
                else
                {
                    // Quoted form: the including file's own directory first --
                    // that is what makes `#include "UnityCG.cginc"` work when
                    // the .cginc sits next to the .shader, regardless of CWD.
                    if (!angle && !current_dir.empty())
                    {
                        candidates.push_back(current_dir / as_written);
                    }
                    for (const auto& root : m_Roots)
                    {
                        candidates.push_back(root / as_written);
                    }
                }

                std::error_code ec;
                for (const auto& candidate : candidates)
                {
                    if (!std::filesystem::is_regular_file(candidate, ec))
                    {
                        continue;
                    }
                    // Opened successfully => found, even when the file is
                    // empty. The old code used `content.empty()` as the
                    // not-found sentinel and so misreported empty headers.
                    out_content = ReadWholeFile(candidate);
                    out_path = candidate;
                    return true;
                }

                return false;
            }

            std::string Searched(const std::string& target,
                                 bool angle,
                                 const std::filesystem::path& current_dir) const
            {
                std::string out;
                const std::filesystem::path as_written(target);
                if (as_written.is_absolute())
                {
                    return as_written.generic_string();
                }
                if (!angle && !current_dir.empty())
                {
                    out += (current_dir / as_written).generic_string();
                }
                for (const auto& root : m_Roots)
                {
                    if (!out.empty())
                    {
                        out += ", ";
                    }
                    out += (root / as_written).generic_string();
                }
                return out.empty() ? "(no search directories configured)" : out;
            }

            std::string Where(int line_no) const
            {
                if (m_Stack.empty())
                {
                    return "line " + std::to_string(line_no);
                }
                return m_Stack.back().generic_string() + ":" + std::to_string(line_no);
            }

            static std::string Quoted(const std::string& target, bool angle)
            {
                return angle ? ("<" + target + ">") : ("\"" + target + "\"");
            }

            std::vector<std::filesystem::path> m_Roots;
            std::vector<std::filesystem::path> m_Stack;   // currently open files
            std::vector<std::filesystem::path> m_Included;
            std::set<std::filesystem::path> m_Once;
            std::string m_Error;
        };

    }  // namespace

    IncludeResolution ResolveIncludes(const std::string& source,
                                      const std::vector<std::string>& include_paths,
                                      const std::filesystem::path& owning_file_dir)
    {
        std::vector<std::filesystem::path> roots;
        roots.reserve(include_paths.size());
        for (const auto& p : include_paths)
        {
            if (!p.empty())
            {
                roots.emplace_back(p);
            }
        }

        IncludeExpander expander(std::move(roots));

        std::string out;
        out.reserve(source.size() * 2);

        IncludeResolution result;
        if (!expander.Expand(source, owning_file_dir, 0, out))
        {
            result.error_message = expander.Error();
            return result;
        }

        result.code = std::move(out);
        result.included_files = expander.IncludedFiles();
        result.success = true;
        return result;
    }

}  // namespace ZEngine::ShaderLab
