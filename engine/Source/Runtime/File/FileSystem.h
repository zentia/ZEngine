#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/JsonSerialize/JSONUtility.h"
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
        try
        {
            AssetType* obj = MemoryManager::CreateObject<AssetType>();
            JSONRead reader(json_str.c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);

            JSONUtility::DeserializedFromJSONRead(reader, *obj);
            reader.ResetCurrentNodeToOriginalState();
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(
                ZFileSystem, "parse json file {} failed: {}!", asset_path.string(), Encoding::GetExceptionMessage(e));
            return false;
        }

        // Serializer::read(asset_json, out_asset);
        return true;
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

    FileSystemHandler* GetHandlerForPath(std::filesystem::path& path) const;

    static bool ReadStringFromFile(std::string* outData, std::filesystem::path& path);

private:
    FileSystemHandler* m_DefaultFileSystem;
    LocalFileSystemHandler* m_LocalFileSystem;
};
