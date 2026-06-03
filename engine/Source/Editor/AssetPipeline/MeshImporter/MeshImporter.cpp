#include "MeshImporter.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorWindow/PreviewWindow/MeshDataPreview.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/MeshData.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <random>
#include <sstream>
#include <vector>

#if defined(ZENGINE_HAS_UFBX)

    #include <ufbx.h>

#endif

#include <tiny_obj_loader.h>

namespace

{

    std::string ToLowerExtension(const std::filesystem::path& path)

    {
        std::string ext = path.extension().generic_string();

        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        return ext;
    }

    bool IsSupportedMeshExtension(const std::string& ext_lower)

    {
        return ext_lower == ".fbx" || ext_lower == ".obj" || ext_lower == ".gltf" || ext_lower == ".glb";
    }

    std::string GenerateGuid()

    {
        std::random_device rd;

        std::mt19937 gen(rd());

        std::uniform_int_distribution<> dis(0, 15);

        std::uniform_int_distribution<> dis2(8, 11);

        std::stringstream ss;

        ss << std::hex;

        for (int i = 0; i < 8; ++i)

            ss << dis(gen);

        ss << "-";

        for (int i = 0; i < 4; ++i)

            ss << dis(gen);

        ss << "-4";

        for (int i = 0; i < 3; ++i)

            ss << dis(gen);

        ss << "-";

        ss << dis2(gen);

        for (int i = 0; i < 3; ++i)

            ss << dis(gen);

        ss << "-";

        for (int i = 0; i < 12; ++i)

            ss << dis(gen);

        return ss.str();
    }

    void RecenterMeshAtOrigin(MeshData& mesh)
    {
        if (mesh.vertex_buffer.empty())
        {
            return;
        }

        AxisAlignedBox bounds;
        for (const Vertex& vertex : mesh.vertex_buffer)
        {
            bounds.Merge(Vector3(vertex.px, vertex.py, vertex.pz));
        }
        if (!bounds.IsValid())
        {
            return;
        }

        const Vector3 center = bounds.getCenter();
        for (Vertex& vertex : mesh.vertex_buffer)
        {
            vertex.px -= center.x;
            vertex.py -= center.y;
            vertex.pz -= center.z;
        }
    }

    void ApplyAxisFlip(MeshData& mesh, bool flip_y)

    {
        if (!flip_y)

        {
            return;
        }

        for (Vertex& v : mesh.vertex_buffer)

        {
            v.py = -v.py;

            v.ny = -v.ny;

            v.ty = -v.ty;

            v.tz = -v.tz;
        }
    }

    bool ImportObjWithTinyObj(const std::filesystem::path& source_path,

                              float scale,

                              MeshData& out_mesh,

                              std::string& out_error)

    {
        tinyobj::ObjReader reader;

        tinyobj::ObjReaderConfig reader_config;

        reader_config.vertex_color = false;

        reader_config.triangulate = true;

        if (!reader.ParseFromFile(source_path.generic_string(), reader_config))

        {
            out_error = reader.Error().empty() ? "tinyobj failed to parse OBJ" : reader.Error();

            return false;
        }

        const auto& attrib = reader.GetAttrib();

        const auto& shapes = reader.GetShapes();

        for (const tinyobj::shape_t& shape : shapes)

        {
            size_t index_offset = 0;

            for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)

            {
                const size_t fv = static_cast<size_t>(shape.mesh.num_face_vertices[f]);

                if (fv < 3)

                {
                    index_offset += fv;

                    continue;
                }

                for (size_t tri = 1; tri + 1 < fv; ++tri)

                {
                    const size_t tri_indices[3] = {0, tri, tri + 1};

                    for (size_t corner = 0; corner < 3; ++corner)

                    {
                        const tinyobj::index_t idx = shape.mesh.indices[index_offset + tri_indices[corner]];

                        Vertex vert {};

                        vert.px = static_cast<float>(attrib.vertices[3 * idx.vertex_index + 0]) * scale;

                        vert.py = static_cast<float>(attrib.vertices[3 * idx.vertex_index + 1]) * scale;

                        vert.pz = static_cast<float>(attrib.vertices[3 * idx.vertex_index + 2]) * scale;

                        if (idx.normal_index >= 0)

                        {
                            vert.nx = static_cast<float>(attrib.normals[3 * idx.normal_index + 0]);

                            vert.ny = static_cast<float>(attrib.normals[3 * idx.normal_index + 1]);

                            vert.nz = static_cast<float>(attrib.normals[3 * idx.normal_index + 2]);
                        }

                        else

                        {
                            vert.ny = 1.0f;
                        }

                        if (idx.texcoord_index >= 0)

                        {
                            vert.u = static_cast<float>(attrib.texcoords[2 * idx.texcoord_index + 0]);

                            vert.v = static_cast<float>(attrib.texcoords[2 * idx.texcoord_index + 1]);
                        }

                        vert.tx = 1.0f;

                        const int vertex_index = static_cast<int>(out_mesh.vertex_buffer.size());

                        out_mesh.vertex_buffer.push_back(vert);

                        out_mesh.index_buffer.push_back(vertex_index);
                    }
                }

                index_offset += fv;
            }
        }

        if (out_mesh.vertex_buffer.empty() || out_mesh.index_buffer.empty())

        {
            out_error = "OBJ contained no triangle geometry";

            return false;
        }

        return true;
    }

#if defined(ZENGINE_HAS_UFBX)

    Vertex VertexFromUfbxIndex(const ufbx_mesh* mesh, size_t index, float scale)

    {
        Vertex vert {};

        const ufbx_vec3 pos = ufbx_get_vertex_vec3(&mesh->vertex_position, index);

        vert.px = static_cast<float>(pos.x) * scale;

        vert.py = static_cast<float>(pos.y) * scale;

        vert.pz = static_cast<float>(pos.z) * scale;

        if (mesh->vertex_normal.exists)

        {
            const ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);

            vert.nx = static_cast<float>(n.x);

            vert.ny = static_cast<float>(n.y);

            vert.nz = static_cast<float>(n.z);
        }

        else

        {
            vert.ny = 1.0f;
        }

        if (mesh->vertex_tangent.exists)

        {
            const ufbx_vec3 t = ufbx_get_vertex_vec3(&mesh->vertex_tangent, index);

            vert.tx = static_cast<float>(t.x);

            vert.ty = static_cast<float>(t.y);

            vert.tz = static_cast<float>(t.z);
        }

        else

        {
            vert.tx = 1.0f;
        }

        if (mesh->vertex_uv.exists)

        {
            const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);

            vert.u = static_cast<float>(uv.x);

            vert.v = static_cast<float>(uv.y);
        }

        return vert;
    }

    bool ImportFbxWithUfbx(const std::filesystem::path& source_path,

                           const MeshImporterSettings& settings,

                           MeshData& out_mesh,

                           std::string& out_error)

    {
        ufbx_load_opts opts = {};

        opts.generate_missing_normals = true;

        opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;

        ufbx_error err = {};

        ufbx_scene* scene = ufbx_load_file(source_path.generic_string().c_str(), &opts, &err);

        if (scene == nullptr)

        {
            char buf[512] = {};

            ufbx_format_error(buf, sizeof(buf), &err);

            out_error = buf[0] != '\0' ? buf : "ufbx failed to load FBX";

            return false;
        }

        out_mesh.vertex_buffer.clear();

        out_mesh.index_buffer.clear();

        out_mesh.bind.clear();

        std::vector<uint32_t> tri_indices;

        for (size_t mesh_i = 0; mesh_i < scene->meshes.count; ++mesh_i)

        {
            const ufbx_mesh* mesh = scene->meshes.data[mesh_i];

            if (mesh == nullptr || !mesh->vertex_position.exists || mesh->num_indices == 0)

            {
                continue;
            }

            const size_t tri_cap = mesh->max_face_triangles > 0 ? mesh->max_face_triangles * 3 : 3;

            if (tri_indices.size() < tri_cap)

            {
                tri_indices.resize(tri_cap);
            }

            for (size_t face_i = 0; face_i < mesh->faces.count; ++face_i)

            {
                const ufbx_face face = mesh->faces.data[face_i];

                if (face.num_indices < 3)

                {
                    continue;
                }

                const uint32_t num_tri_indices =

                    ufbx_triangulate_face(tri_indices.data(), tri_indices.size(), mesh, face);

                for (uint32_t ti = 0; ti < num_tri_indices; ++ti)

                {
                    const size_t ix = static_cast<size_t>(tri_indices[ti]);

                    out_mesh.vertex_buffer.push_back(VertexFromUfbxIndex(mesh, ix, settings.scale));

                    out_mesh.index_buffer.push_back(static_cast<int>(out_mesh.vertex_buffer.size() - 1));
                }
            }
        }

        ufbx_free_scene(scene);

        if (out_mesh.vertex_buffer.empty() || out_mesh.index_buffer.empty())

        {
            out_error = "FBX contained no triangle geometry";

            return false;
        }

        RecenterMeshAtOrigin(out_mesh);
        ApplyAxisFlip(out_mesh, settings.flip_y);

        return true;
    }

#endif

}  // namespace

bool MeshImporter::CanImport(const std::filesystem::path& file_path) const

{
    return IsSupportedMeshExtension(ToLowerExtension(file_path));
}

std::vector<std::string> MeshImporter::GetSupportedExtensions() const

{
    return {".fbx", ".obj", ".gltf", ".glb"};
}

bool MeshImporter::Import(const std::filesystem::path& source_path,

                          const std::filesystem::path& output_path,

                          const AssetImporterSettings& import_settings,

                          AssetMetadata& out_metadata)

{
    if (!std::filesystem::exists(source_path))

    {
        LOG_ERROR(ZEditor, "MeshImporter: source does not exist: {}", source_path.string());

        return false;
    }

    const MeshImporterSettings* mesh_settings = dynamic_cast<const MeshImporterSettings*>(&import_settings);

    if (mesh_settings == nullptr)

    {
        LOG_ERROR(ZEditor, "MeshImporter: invalid import settings type");

        return false;
    }

    const std::string ext_lower = ToLowerExtension(source_path);

    if (!IsSupportedMeshExtension(ext_lower))

    {
        LOG_ERROR(ZEditor, "MeshImporter: unsupported extension {}", ext_lower);

        return false;
    }

    MeshData mesh_geometry;

    std::string error;

    bool ok = false;

    if (ext_lower == ".fbx")

    {
#if defined(ZENGINE_HAS_UFBX)

        ok = ImportFbxWithUfbx(source_path, *mesh_settings, mesh_geometry, error);

#else

        error = "FBX import requires ufbx (place ufbx.c/ufbx.h under engine/3rdparty/ufbx/)";

#endif
    }

    else if (ext_lower == ".obj")

    {
        ok = ImportObjWithTinyObj(source_path, mesh_settings->scale, mesh_geometry, error);

        if (ok)

        {
            RecenterMeshAtOrigin(mesh_geometry);
            ApplyAxisFlip(mesh_geometry, mesh_settings->flip_y);
        }
    }

    else

    {
        error = "glTF/GLB import is not implemented yet";
    }

    if (!ok)

    {
        LOG_ERROR(ZEditor, "MeshImporter: failed to import {}: {}", source_path.string(), error);

        return false;
    }

    if (mesh_geometry.vertex_buffer.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max()))

    {
        LOG_WARNING(ZEditor,

                    "MeshImporter: {} has {} vertices (>65535); runtime still uses 16-bit indices",

                    source_path.string(),

                    mesh_geometry.vertex_buffer.size());
    }

    auto* object_manager = GET_SYSTEM(ObjectManager);

    if (object_manager == nullptr)

    {
        LOG_ERROR(ZEditor, "MeshImporter: ObjectManager unavailable");

        return false;
    }

    Object* produced = object_manager->Produce(TypeOf<MeshData>(), 0);

    if (produced == nullptr)

    {
        LOG_ERROR(ZEditor, "MeshImporter: failed to allocate MeshData");

        return false;
    }

    auto* mesh_object = static_cast<MeshData*>(produced);

    mesh_object->vertex_buffer = std::move(mesh_geometry.vertex_buffer);

    mesh_object->index_buffer = std::move(mesh_geometry.index_buffer);

    mesh_object->bind = std::move(mesh_geometry.bind);

    auto asset_manager = GET_SYSTEM(AssetManager);

    if (asset_manager == nullptr)

    {
        MemoryManager::DestroyObject(produced);

        LOG_ERROR(ZEditor, "MeshImporter: AssetManager unavailable");

        return false;
    }

    {
        std::error_code ec;

        if (!output_path.parent_path().empty())

        {
            std::filesystem::create_directories(output_path.parent_path(), ec);
        }
    }

    const size_t vertex_count = mesh_object->vertex_buffer.size();

    const size_t index_count = mesh_object->index_buffer.size();

    const bool write_ok = asset_manager->WriteObjectToDiskThreadSafe(output_path, *mesh_object);

    if (import_settings.generate_guid && !import_settings.custom_guid.empty())

    {
        out_metadata.guid = import_settings.custom_guid;
    }

    else

    {
        out_metadata.guid = GenerateGuid();
    }

    out_metadata.source_file_path = source_path.generic_string();

    {
        std::error_code ec;

        out_metadata.source_file_time = std::filesystem::last_write_time(source_path, ec);
    }

    out_metadata.dependencies.clear();

    out_metadata.custom_metadata.clear();

    MemoryManager::DestroyObject(produced);

    if (write_ok)

    {
        MeshDataPreview::InvalidatePreview(output_path);

        LOG_INFO(ZEditor,

                 "MeshImporter: {} -> {} (vertices={}, indices={})",

                 source_path.generic_string(),

                 output_path.generic_string(),

                 vertex_count,

                 index_count);
    }

    else

    {
        LOG_ERROR(ZEditor, "MeshImporter: failed to write {}", output_path.string());
    }

    return write_ok;
}

bool MeshImporter::Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings)

{
    if (auto editor_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))

    {
        return editor_mgr->reimportAsset(zasset_path.generic_string(), &import_settings);
    }

    LOG_WARNING(ZEditor, "MeshImporter::Reimport({}): EditorAssetManager unavailable", zasset_path.string());

    return false;
}

std::unique_ptr<AssetImporterSettings> MeshImporter::GetDefaultSettings() const

{
    return std::make_unique<MeshImporterSettings>();
}
