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
#include <string>

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
     * @brief Read an authored TEXT asset, preferring a mounted .zbundle.
     *
     * PR-PAK1 follow-up: every JSON/YAML loader in this header used to be a
     * plain fopen(), so a packaged player could only find global configs,
     * materials and skeletons by falling back to the developer's source tree.
     * SerializedFile (binary .zasset) and AssetManager::ReadObjectsFromYaml()
     * already ask AssetBundleManager first; this is the missing third reader.
     * Bundle wins over disk by design (AGENTS.md 2.24, rule 3). Implemented
     * out-of-line in FileSystem.cpp so this widely-included template header
     * does not have to pull in AssetBundleManager.h.
     *
     * Returns false only when NO mounted bundle has the asset AND the file
     * cannot be opened. An empty file / empty bundle entry is a valid read.
     */
    static bool ReadAssetText(const std::filesystem::path& asset_path, std::string& out_text);

    /**
     * @brief Load asset from JSON file
     */
    template<typename AssetType>
    AssetType* loadAssetJson(const std::filesystem::path& asset_path) const
    {
        // read json file to string (mounted bundle first, then disk)
        std::string json_str;
        if (!ReadAssetText(asset_path, json_str))
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", asset_path.string());
            return nullptr;
        }

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
        std::string json_str;
        if (!ReadAssetText(asset_path, json_str))
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", asset_path.string());
            return nullptr;
        }

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
        std::string buffer;
        if (!ReadAssetText(assetPath, buffer))
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", assetPath.string());
            return false;
        }

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
        std::string buffer;
        if (!ReadAssetText(asset_path, buffer))
        {
            LOG_ERROR(ZFileSystem, "open file {} failed!", asset_path.string());
            return false;
        }

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
