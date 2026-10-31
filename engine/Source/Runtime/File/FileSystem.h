#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/JsonSerialize/JSONUtility.h"
#include "Runtime/Core/YamlSerialize/YAMLUtility.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Resource/Config/ConfigManager.h"

#include <cstdio>
#include <filesystem>

struct WriteData;
class FileSystemHandler;
class LocalFileSystemHandler;
class SerializedFile;

class FileSystem : public IEngineSystem
{
public:
    FileSystem();

    bool Initialize() override { return true; }
    void Shutdown() override {}
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }

    std::vector<std::filesystem::path> GetFiles(const std::filesystem::path& directory);

    /**
     * @brief Load asset from JSON file
     */
    template<typename AssetType>
    AssetType* loadAssetJson(const std::filesystem::path& asset_path) const
    {
        // read json file to string
        FILE* f = fopen(asset_path.string().c_str(), "rb");
        if (!f)
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", asset_path.string());
            return nullptr;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        std::string json_str;
        if (fsz > 0)
        {
            json_str.resize(static_cast<size_t>(fsz));
            fseek(f, 0, SEEK_SET);
            fread(json_str.data(), 1, static_cast<size_t>(fsz), f);
        }
        fclose(f);

        // parse to json object and read to runtime res object
        //
        // ⚠️ 这里原来是 `return true;` / `return false;` —— 而返回类型是
        // `AssetType*`。`true` 会转成 `(AssetType*)1`：一个**非空但完全无效**的
        // 指针。调用方（AssetManager::loadAsset<T>）拿到它只会判空，于是"加载
        // 成功"、一解引用就崩，而且刚 CreateObject 出来的那个对象还泄漏了。
        // 表现为"读 JSON 素材偶发崩溃"，且没有任何日志 —— 因为错误分支那个
        // `return false` 同样被解释成了 `(AssetType*)0`（恰好是 nullptr，反而
        // 看起来"对"）。
        try
        {
            AssetType* obj = MemoryManager::CreateObject<AssetType>();
            JSONRead reader(json_str.c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);

            JSONUtility::DeserializedFromJSONRead(reader, *obj);
            reader.ResetCurrentNodeToOriginalState();
            return obj;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(
                ZFileSystem, "parse json file {} failed: {}!", asset_path.string(), Encoding::GetExceptionMessage(e));
            return nullptr;
        }
    }

    template<typename T, typename... Args>
    T* loadAssetJson(const std::filesystem::path& asset_path, Args&&... args)
    {
        FILE* f = fopen(asset_path.string().c_str(), "rb");
        if (!f)
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", asset_path.string());
            return nullptr;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        std::string json_str;
        if (fsz > 0)
        {
            json_str.resize(static_cast<size_t>(fsz));
            fseek(f, 0, SEEK_SET);
            fread(json_str.data(), 1, static_cast<size_t>(fsz), f);
        }
        fclose(f);

        try
        {
            T* obj = MemoryManager::CreateObject<T>();
            JSONRead reader(json_str.c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);

            JSONUtility::DeserializedFromJSONRead(reader, *obj);
            reader.ResetCurrentNodeToOriginalState();
            return obj;
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(
                ZFileSystem, "parse json file {} failed: {}!", asset_path.string(), Encoding::GetExceptionMessage(e));
            return nullptr;
        }
        return nullptr;
    }

    template<typename T, typename... Args>
    bool LoadAssetByJson(T* obj, const std::filesystem::path& assetPath, Args&&... args)
    {
        FILE* f = fopen(assetPath.string().c_str(), "rb");
        if (!f)
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", assetPath.string());
            return false;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        std::string buffer;
        if (fsz > 0)
        {
            buffer.resize(static_cast<size_t>(fsz));
            fseek(f, 0, SEEK_SET);
            fread(buffer.data(), 1, static_cast<size_t>(fsz), f);
        }
        fclose(f);

        JSONRead reader(buffer.c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);
        JSONUtility::DeserializedFromJSONRead(reader, *obj);
        reader.ResetCurrentNodeToOriginalState();
        return true;
    }

    /**
     * @brief Save asset to JSON file
     */
    template<typename AssetType>
    bool saveAssetJson(const AssetType& in_asset, const std::filesystem::path& asset_path) const
    {
        FILE* f = fopen(asset_path.string().c_str(), "wb");
        if (!f)
        {
            LOG_ERROR(ZFileSystem, "open file {} failed!", asset_path.string());
            return false;
        }

        // write to json object and dump to string
        eastl::string output;
        JSONUtility::SerializeToJSON(in_asset, output);

        // write to file
        fwrite(output.c_str(), 1, output.length(), f);
        fclose(f);

        return true;
    }

    /**
     * @brief Load asset from a block-YAML file
     *
     * Peer of LoadAssetByJson for the text authoring format. Anything that
     * round-trips through Transfer() reads back identically; only the on-disk
     * encoding differs.
     */
    template<typename T, typename... Args>
    bool LoadAssetByYaml(T* obj, const std::filesystem::path& asset_path, Args&&... args)
    {
        FILE* f = fopen(asset_path.string().c_str(), "rb");
        if (!f)
        {
            LOG_ERROR(ZFileSystem, "open file {} failed!", asset_path.string());
            return false;
        }

        fseek(f, 0, SEEK_END);
        const long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string buffer;
        if (file_size > 0)
        {
            buffer.resize(static_cast<size_t>(file_size));
            const size_t read_bytes = fread(buffer.data(), 1, static_cast<size_t>(file_size), f);
            buffer.resize(read_bytes);
        }
        fclose(f);

        try
        {
            YAMLRead reader(buffer.c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);
            YAMLUtility::DeserializeFromYAMLRead(reader, *obj);
            reader.ResetCurrentNodeToOriginalState();
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(ZFileSystem,
                      "parse yaml file {} failed: {}!",
                      asset_path.string(),
                      Encoding::GetExceptionMessage(e));
            return false;
        }

        return true;
    }

    /**
     * @brief Save asset to a block-YAML file
     */
    template<typename AssetType>
    bool saveAssetYaml(const AssetType& in_asset, const std::filesystem::path& asset_path) const
    {
        FILE* f = fopen(asset_path.string().c_str(), "wb");
        if (!f)
        {
            LOG_ERROR(ZFileSystem, "open file {} failed!", asset_path.string());
            return false;
        }

        eastl::string output;
        YAMLUtility::SerializeToYAML(in_asset, output);

        fwrite(output.c_str(), 1, output.length(), f);
        fclose(f);

        return true;
    }

    FileSystemHandler* GetHandlerForPath(std::filesystem::path& path) const;

    static bool ReadStringFromFile(std::string* outData, std::filesystem::path& path);

private:
    FileSystemHandler* m_DefaultFileSystem;
    LocalFileSystemHandler* m_LocalFileSystem;
};
