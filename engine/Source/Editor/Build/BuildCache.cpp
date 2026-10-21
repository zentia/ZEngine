#include "Editor/Build/BuildCache.h"

#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace
{
    constexpr uint64_t kFnvPrime = 0x100000001b3ull;

    std::string ToHex64(uint64_t value)
    {
        static const char* const kDigits = "0123456789abcdef";
        std::string out(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            out[static_cast<std::size_t>(i)] = kDigits[value & 0xFu];
            value >>= 4;
        }
        return out;
    }

    bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& text, std::string& error)
    {
        std::error_code ec;
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec)
            {
                error = "cannot create " + path.parent_path().generic_string() + ": " + ec.message();
                return false;
            }
        }

        // Same temp+rename dance PlayerSettings uses: a crash or a full disk
        // must not leave a half-written cache behind, and a half-written cache
        // is still VALID json for us -- it would just silently claim artifacts
        // are up to date. Write aside, then move into place.
        const std::filesystem::path temp = path.generic_string() + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                error = "cannot write " + temp.generic_string();
                return false;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out)
            {
                error = "cannot write " + temp.generic_string();
                return false;
            }
        }

        std::filesystem::remove(path, ec);
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            // Windows refuses to rename over an existing file in some cases.
            std::error_code copy_ec;
            std::filesystem::copy_file(temp, path, std::filesystem::copy_options::overwrite_existing, copy_ec);
            std::filesystem::remove(temp, ec);
            if (copy_ec)
            {
                error = "cannot move the cache into place: " + copy_ec.message();
                return false;
            }
        }
        return true;
    }

    const char* StringMember(const rapidjson::Value& object, const char* name)
    {
        if (!object.IsObject())
        {
            return nullptr;
        }
        const auto it = object.FindMember(name);
        if (it == object.MemberEnd() || !it->value.IsString())
        {
            return nullptr;
        }
        return it->value.GetString();
    }
}  // namespace

BuildHasher& BuildHasher::AddBytes(const void* data, const std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        state_ ^= bytes[i];
        state_ *= kFnvPrime;
    }
    return *this;
}

BuildHasher& BuildHasher::Add(const std::string& text)
{
    AddBytes(text.data(), text.size());
    // Separator: without it ("ab", "c") and ("a", "bc") hash the same.
    AddBytes("|", 1);
    return *this;
}

std::string BuildHasher::Hex() const
{
    return ToHex64(state_);
}

BuildFileStamp BuildStampOf(const std::filesystem::path& path)
{
    BuildFileStamp stamp;
    std::error_code ec;
    const auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        return stamp;
    }
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return stamp;
    }

    stamp.exists = true;
    stamp.size = size;
    stamp.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();
    return stamp;
}

std::string BuildStampKey(const BuildFileStamp& stamp)
{
    if (!stamp.exists)
    {
        return {};
    }
    return std::to_string(stamp.size) + ":" + std::to_string(stamp.mtime_ns);
}

bool BuildReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        return false;
    }

    const std::streamoff size = in.tellg();
    if (size < 0)
    {
        return false;
    }

    in.seekg(0, std::ios::beg);
    out_bytes.resize(static_cast<std::size_t>(size));
    if (size > 0)
    {
        in.read(reinterpret_cast<char*>(out_bytes.data()), size);
    }
    return static_cast<std::size_t>(in.gcount()) == out_bytes.size();
}

void BuildCache::Reset()
{
    environment.clear();
    inputs.clear();
    outputs.clear();
    loaded_ = true;
}

bool BuildCache::Load(const std::filesystem::path& path, std::string& error)
{
    environment.clear();
    inputs.clear();
    outputs.clear();
    loaded_ = false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
    {
        // First build. Not an error -- the caller rebuilds everything.
        loaded_ = true;
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open the cache";
        return false;
    }

    rapidjson::IStreamWrapper wrapper(in);
    rapidjson::Document document;
    document.ParseStream(wrapper);
    if (document.HasParseError() || !document.IsObject())
    {
        error = "the cache is not valid JSON; rebuilding from scratch";
        return false;
    }

    const auto version_it = document.FindMember("version");
    if (version_it == document.MemberEnd() || !version_it->value.IsInt() ||
        version_it->value.GetInt() != kVersion)
    {
        error = "the cache was written by another version; rebuilding from scratch";
        return false;
    }

    if (const char* value = StringMember(document, "environment"))
    {
        environment = value;
    }

    if (const auto it = document.FindMember("inputs"); it != document.MemberEnd() && it->value.IsObject())
    {
        for (const auto& member : it->value.GetObject())
        {
            Entry entry;
            if (const char* stamp = StringMember(member.value, "stamp"))
            {
                entry.stamp = stamp;
            }
            if (const char* hash = StringMember(member.value, "hash"))
            {
                entry.hash = hash;
            }
            inputs.emplace(member.name.GetString(), std::move(entry));
        }
    }

    if (const auto it = document.FindMember("outputs"); it != document.MemberEnd() && it->value.IsObject())
    {
        for (const auto& member : it->value.GetObject())
        {
            Output output;
            if (const char* fingerprint = StringMember(member.value, "fingerprint"))
            {
                output.fingerprint = fingerprint;
            }
            if (const auto size_it = member.value.FindMember("size_bytes");
                size_it != member.value.MemberEnd() && size_it->value.IsInt64())
            {
                output.size_bytes = size_it->value.GetInt64();
            }
            outputs.emplace(member.name.GetString(), std::move(output));
        }
    }

    loaded_ = true;
    return true;
}

bool BuildCache::Save(const std::filesystem::path& path, std::string& error) const
{
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    // Stable output: pretty-printed and sorted, so the file itself can be
    // diffed between two builds when someone has to debug a rebuild storm.
    writer.StartObject();
    writer.Key("version");
    writer.Int(kVersion);
    writer.Key("environment");
    writer.String(environment.c_str());

    writer.Key("inputs");
    writer.StartObject();
    std::vector<const std::pair<const std::string, Entry>*> sorted_inputs;
    sorted_inputs.reserve(inputs.size());
    for (const auto& entry : inputs)
    {
        sorted_inputs.push_back(&entry);
    }
    std::sort(sorted_inputs.begin(), sorted_inputs.end(),
              [](const std::pair<const std::string, Entry>* a, const std::pair<const std::string, Entry>* b) {
                  return a->first < b->first;
              });
    for (const auto* entry : sorted_inputs)
    {
        writer.Key(entry->first.c_str());
        writer.StartObject();
        writer.Key("stamp");
        writer.String(entry->second.stamp.c_str());
        writer.Key("hash");
        writer.String(entry->second.hash.c_str());
        writer.EndObject();
    }
    writer.EndObject();

    writer.Key("outputs");
    writer.StartObject();
    std::vector<const std::pair<const std::string, Output>*> sorted_outputs;
    sorted_outputs.reserve(outputs.size());
    for (const auto& output : outputs)
    {
        sorted_outputs.push_back(&output);
    }
    std::sort(sorted_outputs.begin(), sorted_outputs.end(),
              [](const std::pair<const std::string, Output>* a, const std::pair<const std::string, Output>* b) {
                  return a->first < b->first;
              });
    for (const auto* output : sorted_outputs)
    {
        writer.Key(output->first.c_str());
        writer.StartObject();
        writer.Key("fingerprint");
        writer.String(output->second.fingerprint.c_str());
        writer.Key("size_bytes");
        writer.Int64(output->second.size_bytes);
        writer.EndObject();
    }
    writer.EndObject();
    writer.EndObject();

    return WriteTextFileAtomic(path, std::string(buffer.GetString(), buffer.GetSize()), error);
}

std::string BuildCache::ContentHash(const std::string& key, const std::filesystem::path& file, bool& ok)
{
    ok = false;

    const BuildFileStamp stamp = BuildStampOf(file);
    const std::string stamp_key = BuildStampKey(stamp);
    if (stamp_key.empty())
    {
        // Gone, or unreadable. Drop what we knew so a later build cannot
        // reuse a hash for a file that is no longer there.
        inputs.erase(key);
        return {};
    }

    if (const auto it = inputs.find(key);
        it != inputs.end() && it->second.stamp == stamp_key && !it->second.hash.empty())
    {
        ok = true;
        return it->second.hash;
    }

    std::vector<uint8_t> bytes;
    if (!BuildReadWholeFile(file, bytes))
    {
        inputs.erase(key);
        return {};
    }

    ok = true;
    return ContentHashKnown(key, stamp_key, bytes);
}

std::string BuildCache::ContentHashKnown(const std::string& key, const std::string& stamp_key,
                                         const std::vector<uint8_t>& bytes)
{
    BuildHasher hasher;
    hasher.AddBytes(bytes.data(), bytes.size());
    const std::string hash = hasher.Hex();

    Entry& entry = inputs[key];
    entry.hash = hash;
    entry.stamp = stamp_key;
    return hash;
}

bool BuildCache::IsUpToDate(const std::string& key, const std::string& fingerprint,
                            const std::filesystem::path& artifact) const
{
    if (!loaded_ || fingerprint.empty())
    {
        return false;
    }

    const auto it = outputs.find(key);
    if (it == outputs.end() || it->second.fingerprint != fingerprint)
    {
        return false;
    }

    // The artifact is the real deliverable: if someone deleted it (or cleaned
    // Build/) the cache has to lose, or the player ships a missing bundle.
    std::error_code ec;
    if (!std::filesystem::exists(artifact, ec) || ec)
    {
        return false;
    }
    const uint64_t size = std::filesystem::file_size(artifact, ec);
    if (ec || static_cast<int64_t>(size) != it->second.size_bytes)
    {
        return false;
    }
    return true;
}

void BuildCache::Record(const std::string& key, const std::string& fingerprint, const int64_t size_bytes)
{
    Output& output = outputs[key];
    output.fingerprint = fingerprint;
    output.size_bytes = size_bytes;
}

void BuildCache::Erase(const std::string& key)
{
    outputs.erase(key);
}

std::vector<std::string> BuildCache::StaleKeys(const std::vector<std::string>& current_keys) const
{
    std::vector<std::string> stale;
    for (const auto& [key, output] : outputs)
    {
        if (std::find(current_keys.begin(), current_keys.end(), key) == current_keys.end())
        {
            stale.push_back(key);
        }
    }
    std::sort(stale.begin(), stale.end());
    return stale;
}
