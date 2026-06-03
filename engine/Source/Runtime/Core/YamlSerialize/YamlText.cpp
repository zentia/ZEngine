#include "Runtime/Core/YamlSerialize/YamlText.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ZYaml
{
namespace
{
    using rapidjson::SizeType;

    bool IsScalar(const YamlValue& v) { return !v.IsObject() && !v.IsArray(); }

    // ---- Emit -----------------------------------------------------------------

    void AppendIndent(std::string& out, int n) { out.append(static_cast<size_t>(n), ' '); }

    bool LooksNumericToken(const char* s, size_t len)
    {
        // optional sign, then a number with at least one digit (int or float with
        // optional fraction / exponent). Used by the emitter to force-quote string
        // scalars that would otherwise re-parse as numbers.
        size_t i = 0;
        if (i < len && (s[i] == '+' || s[i] == '-'))
            ++i;
        bool digit = false;
        while (i < len && s[i] >= '0' && s[i] <= '9')
        {
            digit = true;
            ++i;
        }
        if (i < len && s[i] == '.')
        {
            ++i;
            while (i < len && s[i] >= '0' && s[i] <= '9')
            {
                digit = true;
                ++i;
            }
        }
        if (!digit)
            return false;
        if (i < len && (s[i] == 'e' || s[i] == 'E'))
        {
            ++i;
            if (i < len && (s[i] == '+' || s[i] == '-'))
                ++i;
            bool edigit = false;
            while (i < len && s[i] >= '0' && s[i] <= '9')
            {
                edigit = true;
                ++i;
            }
            if (!edigit)
                return false;
        }
        return i == len;
    }

    bool EqualsIgnoreCase(const char* s, size_t len, const char* word)
    {
        const size_t wl = std::strlen(word);
        if (len != wl)
            return false;
        for (size_t i = 0; i < len; ++i)
        {
            char a = s[i];
            char b = word[i];
            if (a >= 'A' && a <= 'Z')
                a = static_cast<char>(a - 'A' + 'a');
            if (a != b)
                return false;
        }
        return true;
    }

    // A scalar string can be emitted unquoted only if it cannot be confused with
    // another YAML type and contains no characters that would break block parsing.
    bool IsPlainSafe(const char* s, size_t len)
    {
        if (len == 0)
            return false;  // empty -> must be "" so it doesn't read back as null
        if (s[0] == ' ' || s[len - 1] == ' ')
            return false;  // leading/trailing spaces are lost by a plain scalar
        // Reserved scalars that would re-parse as bool / null.
        const char* kReserved[] = {"true", "false", "null", "~", "yes", "no", "on", "off"};
        for (const char* w : kReserved)
        {
            if (EqualsIgnoreCase(s, len, w))
                return false;
        }
        if (LooksNumericToken(s, len))
            return false;
        // Only a conservative character set stays plain. Anything outside it
        // (':', '#', '"', flow indicators, control chars, ...) forces quoting.
        for (size_t i = 0; i < len; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '_' || c == '.' || c == '/' || c == '-' || c == ' ' || c >= 0x80;
            if (!ok)
                return false;
        }
        // A leading '-' followed by space already handled (space rule); a bare
        // leading '-' is fine ("-Foo"), but "- " can't occur here (space set ok yet
        // a "- " prefix is a sequence marker) -- guard it explicitly.
        if (len >= 2 && s[0] == '-' && s[1] == ' ')
            return false;
        return true;
    }

    void EmitQuoted(const char* s, size_t len, std::string& out)
    {
        out += '"';
        for (size_t i = 0; i < len; ++i)
        {
            const char c = s[i];
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default: out += c; break;
            }
        }
        out += '"';
    }

    void EmitString(const char* s, size_t len, std::string& out)
    {
        if (IsPlainSafe(s, len))
            out.append(s, len);
        else
            EmitQuoted(s, len, out);
    }

    void EmitScalarToken(const YamlValue& v, std::string& out)
    {
        if (v.IsNull())
        {
            out += '~';
            return;
        }
        if (v.IsBool())
        {
            out += v.GetBool() ? "true" : "false";
            return;
        }
        if (v.IsInt())
        {
            out += std::to_string(v.GetInt());
            return;
        }
        if (v.IsUint())
        {
            out += std::to_string(v.GetUint());
            return;
        }
        if (v.IsInt64())
        {
            out += std::to_string(v.GetInt64());
            return;
        }
        if (v.IsUint64())
        {
            out += std::to_string(v.GetUint64());
            return;
        }
        if (v.IsDouble())
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", v.GetDouble());
            out += buf;
            return;
        }
        if (v.IsString())
        {
            EmitString(v.GetString(), v.GetStringLength(), out);
            return;
        }
        out += '~';
    }

    void EmitMapping(const YamlValue& obj, int indent, std::string& out);
    void EmitSequence(const YamlValue& arr, int indent, std::string& out);

    // Writes the part that follows a "key:" or "-" marker: either " <scalar>\n",
    // " {}\n" / " []\n" for empty collections, or "\n<block>" for a nested block.
    void EmitChild(const YamlValue& v, int childIndent, std::string& out)
    {
        if (IsScalar(v))
        {
            out += ' ';
            EmitScalarToken(v, out);
            out += '\n';
            return;
        }
        if (v.IsObject())
        {
            if (v.MemberCount() == 0)
            {
                out += " {}\n";
                return;
            }
            out += '\n';
            EmitMapping(v, childIndent, out);
            return;
        }
        // array
        if (v.Empty())
        {
            out += " []\n";
            return;
        }
        out += '\n';
        EmitSequence(v, childIndent, out);
    }

    void EmitMapping(const YamlValue& obj, int indent, std::string& out)
    {
        for (auto m = obj.MemberBegin(); m != obj.MemberEnd(); ++m)
        {
            AppendIndent(out, indent);
            EmitString(m->name.GetString(), m->name.GetStringLength(), out);
            out += ':';
            EmitChild(m->value, indent + 2, out);
        }
    }

    void EmitSequence(const YamlValue& arr, int indent, std::string& out)
    {
        for (auto i = arr.Begin(); i != arr.End(); ++i)
        {
            const YamlValue& e = *i;
            AppendIndent(out, indent);
            out += '-';
            if (e.IsObject() && e.MemberCount() != 0)
            {
                // Inline the first key on the "- " line; remaining keys align at
                // indent+2 (the column right after "- "). Nested blocks of those
                // keys land at indent+4 via EmitChild.
                bool first = true;
                for (auto m = e.MemberBegin(); m != e.MemberEnd(); ++m)
                {
                    if (first)
                    {
                        out += ' ';
                        first = false;
                    }
                    else
                    {
                        AppendIndent(out, indent + 2);
                    }
                    EmitString(m->name.GetString(), m->name.GetStringLength(), out);
                    out += ':';
                    EmitChild(m->value, indent + 4, out);
                }
            }
            else
            {
                EmitChild(e, indent + 2, out);
            }
        }
    }

    // ---- Parse ----------------------------------------------------------------

    class YamlParser
    {
    public:
        YamlParser(const char* text, YamlDocument& doc) : m_Doc(doc), m_Alloc(doc.GetAllocator())
        {
            Tokenize(text);
        }

        bool Parse()
        {
            if (m_Lines.empty())
            {
                static_cast<YamlValue&>(m_Doc).SetObject();
                return true;
            }
            m_Cursor = 0;
            if (m_Lines[m_Cursor].content == "---")
                ++m_Cursor;
            if (m_Cursor >= m_Lines.size())
            {
                static_cast<YamlValue&>(m_Doc).SetObject();
                return true;
            }
            const std::string& first = m_Lines[m_Cursor].content;
            YamlValue root;
            if (first == "{}")
            {
                root.SetObject();
                ++m_Cursor;
            }
            else if (first == "[]")
            {
                root.SetArray();
                ++m_Cursor;
            }
            else
            {
                root = ParseBlock(m_Lines[m_Cursor].indent);
            }
            static_cast<YamlValue&>(m_Doc).Swap(root);
            return true;
        }

    private:
        struct Line
        {
            int indent;
            std::string content;  // leading indent stripped, trailing whitespace stripped
        };

        void Tokenize(const char* text)
        {
            const char* p = text;
            while (*p != '\0')
            {
                const char* start = p;
                while (*p != '\0' && *p != '\n')
                    ++p;
                std::string raw(start, static_cast<size_t>(p - start));
                if (*p == '\n')
                    ++p;
                if (!raw.empty() && raw.back() == '\r')
                    raw.pop_back();

                size_t indent = 0;
                while (indent < raw.size() && raw[indent] == ' ')
                    ++indent;
                if (indent == raw.size())
                    continue;  // blank line
                if (raw[indent] == '#')
                    continue;  // full-line comment

                std::string content = raw.substr(indent);
                while (!content.empty() && (content.back() == ' ' || content.back() == '\t'))
                    content.pop_back();
                m_Lines.push_back(Line {static_cast<int>(indent), std::move(content)});
            }
        }

        static bool IsSeqMarker(const std::string& c)
        {
            return !c.empty() && c[0] == '-' && (c.size() == 1 || c[1] == ' ');
        }

        // Index of the ':' that terminates a mapping key, or npos when the line is
        // not a "key: ..." pair. Handles a double-quoted key and only treats a
        // colon as a separator when it ends the line or is followed by a space
        // (matches the emitter, which quotes any scalar containing ": ").
        static size_t KeyColon(const std::string& c)
        {
            size_t i = 0;
            if (!c.empty() && c[0] == '"')
            {
                ++i;
                while (i < c.size())
                {
                    if (c[i] == '\\')
                        i += 2;
                    else if (c[i] == '"')
                    {
                        ++i;
                        break;
                    }
                    else
                        ++i;
                }
                while (i < c.size() && c[i] == ' ')
                    ++i;
                if (i < c.size() && c[i] == ':')
                    return i;
                return std::string::npos;
            }
            for (; i < c.size(); ++i)
            {
                if (c[i] == ':' && (i + 1 == c.size() || c[i + 1] == ' '))
                    return i;
            }
            return std::string::npos;
        }

        YamlValue ParseBlock(int indent)
        {
            if (IsSeqMarker(m_Lines[m_Cursor].content))
                return ParseSequence(indent);
            return ParseMapping(indent);
        }

        YamlValue ParseMapping(int indent)
        {
            YamlValue obj(rapidjson::kObjectType);
            while (m_Cursor < m_Lines.size() && m_Lines[m_Cursor].indent == indent)
            {
                const std::string& c = m_Lines[m_Cursor].content;
                if (IsSeqMarker(c))
                    break;
                const size_t colon = KeyColon(c);
                if (colon == std::string::npos)
                {
                    ++m_Cursor;  // malformed line; skip defensively
                    continue;
                }
                std::string key = Unquote(c.substr(0, colon));
                std::string rest = c.substr(colon + 1);
                size_t rs = 0;
                while (rs < rest.size() && rest[rs] == ' ')
                    ++rs;
                rest.erase(0, rs);

                ++m_Cursor;
                YamlValue val = ParseInlineOrBlock(rest, indent);

                YamlValue keyNode;
                keyNode.SetString(key.c_str(), static_cast<SizeType>(key.size()), m_Alloc);
                obj.AddMember(keyNode, val, m_Alloc);
            }
            return obj;
        }

        YamlValue ParseSequence(int indent)
        {
            YamlValue arr(rapidjson::kArrayType);
            while (m_Cursor < m_Lines.size() && m_Lines[m_Cursor].indent == indent &&
                   IsSeqMarker(m_Lines[m_Cursor].content))
            {
                const std::string& c = m_Lines[m_Cursor].content;
                std::string after = (c.size() >= 2) ? c.substr(2) : "";
                size_t as = 0;
                while (as < after.size() && after[as] == ' ')
                    ++as;
                after.erase(0, as);

                if (!after.empty() && after != "{}" && after != "[]" && KeyColon(after) != std::string::npos)
                {
                    // Map element with its first pair inline. Rewrite the dash line
                    // into a mapping line at indent+2 and let ParseMapping consume it
                    // plus any sibling keys (which the emitter wrote at indent+2).
                    m_Lines[m_Cursor].indent = indent + 2;
                    m_Lines[m_Cursor].content = after;
                    YamlValue elem = ParseMapping(indent + 2);
                    arr.PushBack(elem, m_Alloc);
                }
                else
                {
                    ++m_Cursor;
                    YamlValue elem = ParseInlineOrBlock(after, indent);
                    arr.PushBack(elem, m_Alloc);
                }
            }
            return arr;
        }

        // Resolve the value that followed a "key:" or "- " marker. `rest` is the
        // text after the marker (already trimmed); `markerIndent` is the indent of
        // the marker line. Empty `rest` means the value is a nested block on the
        // following deeper-indented lines (or null when none follow).
        YamlValue ParseInlineOrBlock(const std::string& rest, int markerIndent)
        {
            YamlValue val;
            if (rest.empty())
            {
                if (m_Cursor < m_Lines.size() && m_Lines[m_Cursor].indent > markerIndent)
                    val = ParseBlock(m_Lines[m_Cursor].indent);
                else
                    val.SetNull();
            }
            else if (rest == "{}")
            {
                val.SetObject();
            }
            else if (rest == "[]")
            {
                val.SetArray();
            }
            else
            {
                val = ParseScalar(rest);
            }
            return val;
        }

        static bool LooksLikeInt(const std::string& t)
        {
            size_t i = 0;
            if (i < t.size() && (t[i] == '+' || t[i] == '-'))
                ++i;
            if (i >= t.size())
                return false;
            for (; i < t.size(); ++i)
            {
                if (t[i] < '0' || t[i] > '9')
                    return false;
            }
            return true;
        }

        YamlValue ParseScalar(const std::string& t)
        {
            YamlValue v;
            if (t.empty())
            {
                v.SetNull();
                return v;
            }
            if (t[0] == '"')
            {
                std::string s = Unquote(t);
                v.SetString(s.c_str(), static_cast<SizeType>(s.size()), m_Alloc);
                return v;
            }
            if (t == "~" || t == "null" || t == "Null" || t == "NULL")
            {
                v.SetNull();
                return v;
            }
            if (t == "true" || t == "True" || t == "TRUE")
            {
                v.SetBool(true);
                return v;
            }
            if (t == "false" || t == "False" || t == "FALSE")
            {
                v.SetBool(false);
                return v;
            }
            if (LooksLikeInt(t))
            {
                char* endp = nullptr;
                const long long ll = std::strtoll(t.c_str(), &endp, 10);
                if (endp != nullptr && *endp == '\0')
                {
                    if (ll >= INT32_MIN && ll <= INT32_MAX)
                        v.SetInt(static_cast<int32_t>(ll));
                    else
                        v.SetInt64(ll);
                    return v;
                }
                const unsigned long long ull = std::strtoull(t.c_str(), &endp, 10);
                if (endp != nullptr && *endp == '\0')
                {
                    v.SetUint64(ull);
                    return v;
                }
            }
            if (LooksNumericToken(t.c_str(), t.size()))
            {
                char* endp = nullptr;
                const double d = std::strtod(t.c_str(), &endp);
                if (endp != nullptr && *endp == '\0')
                {
                    v.SetDouble(d);
                    return v;
                }
            }
            v.SetString(t.c_str(), static_cast<SizeType>(t.size()), m_Alloc);
            return v;
        }

        // Strip surrounding double quotes and unescape. A token that does not start
        // with '"' is returned verbatim (plain scalar / key).
        static std::string Unquote(const std::string& t)
        {
            if (t.empty() || t[0] != '"')
                return t;
            std::string out;
            size_t i = 1;
            while (i < t.size())
            {
                const char c = t[i];
                if (c == '"')
                    break;
                if (c == '\\' && i + 1 < t.size())
                {
                    const char n = t[i + 1];
                    switch (n)
                    {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        default: out += n; break;
                    }
                    i += 2;
                }
                else
                {
                    out += c;
                    ++i;
                }
            }
            return out;
        }

        YamlDocument& m_Doc;
        JSONAllocator& m_Alloc;
        std::vector<Line> m_Lines;
        size_t m_Cursor {0};
    };
}  // namespace

void EmitYaml(const YamlValue& root, eastl::string& out)
{
    std::string s;
    if (root.IsObject())
    {
        if (root.MemberCount() == 0)
            s = "{}\n";
        else
            EmitMapping(root, 0, s);
    }
    else if (root.IsArray())
    {
        if (root.Empty())
            s = "[]\n";
        else
            EmitSequence(root, 0, s);
    }
    else
    {
        EmitScalarToken(root, s);
        s += '\n';
    }
    out.assign(s.data(), s.size());
}

bool ParseYaml(const char* text, YamlDocument& outDoc)
{
    if (text == nullptr)
    {
        static_cast<YamlValue&>(outDoc).SetObject();
        return false;
    }
    YamlParser parser(text, outDoc);
    return parser.Parse();
}
}  // namespace ZYaml
