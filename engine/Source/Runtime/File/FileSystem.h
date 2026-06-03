#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/JsonSerialize/JSONUtility.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Resource/Config/ConfigManager.h"

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
        std::ifstream asset_json_file(asset_path);
        if (!asset_json_file)
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", asset_path.string());
            return false;
        }

        std::stringstream buffer;
        buffer << asset_json_file.rdbuf();

        // parse to json object and read to runtime res object
        try
        {
            AssetType* obj = MemoryManager::CreateObject<AssetType>();
            JSONRead reader(buffer.str().c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);

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
        std::ifstream asset_json_file(asset_path);
        if (!asset_json_file)
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", asset_path.string());
            return nullptr;
        }

        std::stringstream buffer;
        buffer << asset_json_file.rdbuf();

        try
        {
            T* obj = MemoryManager::CreateObject<T>();
            JSONRead reader(buffer.str().c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);

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
        std::ifstream asset_json_file(assetPath);
        if (!asset_json_file)
        {
            LOG_ERROR(ZFileSystem, "open file: {} failed!", assetPath.string());
            return false;
        }

        std::stringstream buffer;
        buffer << asset_json_file.rdbuf();
        JSONRead reader(buffer.str().c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);
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
        std::ofstream asset_json_file(asset_path);
        if (!asset_json_file)
        {
            LOG_ERROR(ZFileSystem, "open file {} failed!", asset_path.string());
            return false;
        }

        // write to json object and dump to string
        eastl::string output;
        JSONUtility::SerializeToJSON(in_asset, output);

        // write to file
        asset_json_file << output.c_str();
        asset_json_file.flush();

        return true;
    }

    FileSystemHandler* GetHandlerForPath(std::filesystem::path& path) const;

    static bool ReadStringFromFile(std::string* outData, std::filesystem::path& path);

private:
    FileSystemHandler* m_DefaultFileSystem;
    LocalFileSystemHandler* m_LocalFileSystem;
};
