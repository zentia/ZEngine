#include "Editor/AssetPipeline/MeshImporter/GltfMeshReader.h"

// cgltf 是 C99，且实现在 cgltf_impl.c 里 —— 这里只要声明。
#include "cgltf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace zgltf
{
    namespace
    {
        const char* ResultName(cgltf_result r)
        {
            switch (r)
            {
                case cgltf_result_success:          return "success";
                case cgltf_result_data_too_short:   return "data too short";
                case cgltf_result_unknown_format:   return "unknown format";
                case cgltf_result_invalid_json:     return "invalid JSON";
                case cgltf_result_invalid_gltf:     return "invalid glTF";
                case cgltf_result_invalid_options:  return "invalid options";
                case cgltf_result_file_not_found:   return "file not found";
                case cgltf_result_io_error:         return "I/O error";
                case cgltf_result_out_of_memory:    return "out of memory";
                case cgltf_result_legacy_gltf:      return "legacy glTF (1.0); only glTF 2.0 is supported";
                default:                            return "unknown error";
            }
        }

        bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out_bytes, std::string& out_error)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                out_error = "cannot open file";
                return false;
            }
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size <= 0)
            {
                out_error = "file is empty or unseekable";
                return false;
            }
            in.seekg(0, std::ios::beg);
            out_bytes.resize(static_cast<size_t>(size));
            in.read(reinterpret_cast<char*>(out_bytes.data()), size);
            if (static_cast<std::streamoff>(in.gcount()) != size)
            {
                out_error = "short read";
                return false;
            }
            return true;
        }

        // ---- 变换 ----
        //
        // cgltf 给的是 OpenGL 风格的**列主序** 4x4：m[col * 4 + row]。
        // 这一点搞反的后果是模型被转置（看着像"随机歪了"），不报错。

        void TransformPoint(const cgltf_float m[16], float x, float y, float z, float out[3])
        {
            out[0] = static_cast<float>(m[0] * x + m[4] * y + m[8]  * z + m[12]);
            out[1] = static_cast<float>(m[1] * x + m[5] * y + m[9]  * z + m[13]);
            out[2] = static_cast<float>(m[2] * x + m[6] * y + m[10] * z + m[14]);
        }

        /// 法线用**逆转置**而不是同一个矩阵。
        ///
        /// 非均匀缩放（(1, 2, 1) 之类）下用原矩阵变换法线，法线就不再垂直于
        /// 表面 —— 光照整片发暗/发亮，而模型"看起来导入成功了"。这是静默错误，
        /// 所以这里老老实实算 3x3 的逆转置。
        void TransformNormal(const cgltf_float m[16], float x, float y, float z, float out[3])
        {
            float a[3][3];
            for (int r = 0; r < 3; ++r)
            {
                for (int c = 0; c < 3; ++c)
                {
                    a[r][c] = static_cast<float>(m[c * 4 + r]);
                }
            }

            const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                            - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                            + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);

            if (std::fabs(det) < 1.0e-12f)
            {
                // 退化（含镜像/零缩放）：退化到"只用旋转部分"，至少方向不会反。
                out[0] = a[0][0] * x + a[0][1] * y + a[0][2] * z;
                out[1] = a[1][0] * x + a[1][1] * y + a[1][2] * z;
                out[2] = a[2][0] * x + a[2][1] * y + a[2][2] * z;
            }
            else
            {
                float inv[3][3];
                inv[0][0] =  (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
                inv[0][1] = -(a[0][1] * a[2][2] - a[0][2] * a[2][1]) / det;
                inv[0][2] =  (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
                inv[1][0] = -(a[1][0] * a[2][2] - a[1][2] * a[2][0]) / det;
                inv[1][1] =  (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
                inv[1][2] = -(a[0][0] * a[1][2] - a[0][2] * a[1][0]) / det;
                inv[2][0] =  (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
                inv[2][1] = -(a[0][0] * a[2][1] - a[0][1] * a[2][0]) / det;
                inv[2][2] =  (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;

                // n' = transpose(inv) * n  ⇒  n'_r = Σ_c inv[c][r] * n_c
                out[0] = inv[0][0] * x + inv[1][0] * y + inv[2][0] * z;
                out[1] = inv[0][1] * x + inv[1][1] * y + inv[2][1] * z;
                out[2] = inv[0][2] * x + inv[1][2] * y + inv[2][2] * z;
            }

            const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
            if (len > 1.0e-12f)
            {
                out[0] /= len;
                out[1] /= len;
                out[2] /= len;
            }
            else
            {
                out[0] = 0.0f;
                out[1] = 1.0f;  // 与 OBJ 导入的缺省法线一致
                out[2] = 0.0f;
            }
        }

        const cgltf_accessor* FindAttribute(const cgltf_primitive& prim, cgltf_attribute_type type, cgltf_int index)
        {
            for (cgltf_size i = 0; i < prim.attributes_count; ++i)
            {
                if (prim.attributes[i].type == type && prim.attributes[i].index == index)
                {
                    return prim.attributes[i].data;
                }
            }
            return nullptr;
        }

        struct NodeContext
        {
            const cgltf_node* node {nullptr};
            cgltf_float       world[16] {};
        };

        bool CollectNodes(const cgltf_node* const* nodes,
                          cgltf_size count,
                          std::vector<NodeContext>& out_nodes)
        {
            for (cgltf_size i = 0; i < count; ++i)
            {
                const cgltf_node* node = nodes[i];
                if (node == nullptr)
                {
                    continue;
                }
                NodeContext ctx;
                ctx.node = node;
                cgltf_node_transform_world(node, ctx.world);
                out_nodes.push_back(ctx);

                if (!CollectNodes(node->children, node->children_count, out_nodes))
                {
                    return false;
                }
            }
            return true;
        }
    }

    bool ReadGltfMeshGeometry(const std::filesystem::path& source_path,
                              float scale,
                              MeshData& out_mesh,
                              std::string& out_error,
                              std::vector<std::string>& out_warnings)
    {
        // 自己把文件读进内存再 cgltf_parse，而不是 cgltf_parse_file。
        //
        // cgltf_parse_file 内部走 fopen(const char*)，在 Windows 上遇到非 ASCII
        // 路径会直接失败（工程/资产路径含中文是常态）。自己读字节既绕开编码
        // 问题，也让我们读失败时能给中文路径无关的统一报错。
        std::vector<uint8_t> file_bytes;
        if (!ReadWholeFile(source_path, file_bytes, out_error))
        {
            out_error = "cannot read source file: " + out_error;
            return false;
        }

        cgltf_options options {};
        cgltf_data* data = nullptr;
        const cgltf_result parsed = cgltf_parse(&options, file_bytes.data(), file_bytes.size(), &data);
        if (parsed != cgltf_result_success)
        {
            out_error = std::string("cgltf parse failed: ") + ResultName(parsed);
            return false;
        }

        // data 的释放必须在所有 return 之前，用 RAII 包一层最省心。
        struct DataGuard
        {
            cgltf_data* ptr {nullptr};
            ~DataGuard() { if (ptr) { cgltf_free(ptr); } }
        } guard {data};

        // 外置 .bin / data: URI 在这一步被解进 data->buffers[i].data。
        // gltf_path 用源文件自己的路径，URI 才能相对它解析。
        const std::string gltf_path = source_path.generic_string();
        const cgltf_result loaded = cgltf_load_buffers(&options, data, gltf_path.c_str());
        if (loaded != cgltf_result_success)
        {
            out_error = std::string("cgltf buffer load failed: ") + ResultName(loaded);
            return false;
        }

        // validate 会抓出越界访问器、错位索引这类"能解析但数据是坏的"情况。
        // 它比 cgltf_parse 严格，可能拒掉个别能凑合用的文件 —— 这是有意的：
        // 静默产出错误几何比直接失败糟糕得多。
        const cgltf_result validated = cgltf_validate(data);
        if (validated != cgltf_result_success)
        {
            out_error = std::string("cgltf validate failed: ") + ResultName(validated);
            return false;
        }

        // 没有任何场景时退回到"全部 mesh"：规范允许不写 scenes，导出器有这种产物。
        std::vector<NodeContext> nodes;
        if (data->scenes_count > 0)
        {
            const cgltf_scene& scene = data->scenes[data->scene != nullptr ? (data->scene - data->scenes) : 0];
            CollectNodes(scene.nodes, scene.nodes_count, nodes);
        }
        if (nodes.empty())
        {
            // 没有场景图时按根节点处理（只取 data->nodes 的顶层）
            std::vector<const cgltf_node*> roots;
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
            {
                roots.push_back(&data->nodes[i]);
            }
            CollectNodes(roots.data(), roots.size(), nodes);
        }

        const size_t vertex_offset = out_mesh.vertex_buffer.size();
        size_t produced_triangles = 0;

        for (const NodeContext& ctx : nodes)
        {
            const cgltf_node* node = ctx.node;
            if (node->mesh == nullptr)
            {
                continue;
            }

            // ❌ 蒙皮：不做。
            //
            // 静默按静态网格导入会得到一个"摆着 T-pose 且不动"的模型 —— 看起来
            // 导入成功了，实际是错的。必须报错。
            if (node->skin != nullptr)
            {
                out_error = "skinned meshes are not supported yet (node references a skin); "
                            "import as static geometry is deliberately refused";
                return false;
            }

            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p)
            {
                const cgltf_primitive& prim = node->mesh->primitives[p];

                if (prim.has_draco_mesh_compression)
                {
                    out_error = "Draco-compressed primitives are not supported";
                    return false;
                }

                if (prim.type != cgltf_primitive_type_triangles)
                {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "unsupported primitive mode %d (only TRIANGLES=%d is supported)",
                                  static_cast<int>(prim.type),
                                  static_cast<int>(cgltf_primitive_type_triangles));
                    out_error = buf;
                    return false;
                }

                if (prim.targets_count > 0)
                {
                    out_warnings.push_back("morph targets ignored (base geometry only)");
                }

                const cgltf_accessor* positions = FindAttribute(prim, cgltf_attribute_type_position, 0);
                if (positions == nullptr)
                {
                    out_error = "primitive has no POSITION attribute";
                    return false;
                }
                const cgltf_accessor* normals   = FindAttribute(prim, cgltf_attribute_type_normal, 0);
                const cgltf_accessor* texcoords = FindAttribute(prim, cgltf_attribute_type_texcoord, 0);

                const cgltf_size vertex_count = positions->count;

                // 索引：有就读，没有就按顶点顺序隐式编号（规范允许的 non-indexed 图元）。
                cgltf_size index_count = vertex_count;
                if (prim.indices != nullptr)
                {
                    index_count = prim.indices->count;
                }

                if (index_count % 3 != 0)
                {
                    out_error = "index count is not a multiple of 3 (not a pure triangle list)";
                    return false;
                }

                for (cgltf_size i = 0; i < index_count; ++i)
                {
                    cgltf_size vi = i;
                    if (prim.indices != nullptr)
                    {
                        cgltf_uint read = 0;
                        if (!cgltf_accessor_read_uint(prim.indices, i, &read, 1))
                        {
                            out_error = "failed to read index";
                            return false;
                        }
                        vi = static_cast<cgltf_size>(read);
                        if (vi >= vertex_count)
                        {
                            out_error = "index out of range of the POSITION accessor";
                            return false;
                        }
                    }

                    Vertex vert {};

                    cgltf_float pos[3] = {0, 0, 0};
                    if (!cgltf_accessor_read_float(positions, vi, pos, 3))
                    {
                        out_error = "failed to read POSITION";
                        return false;
                    }
                    float tp[3] = {0, 0, 0};
                    TransformPoint(ctx.world, static_cast<float>(pos[0]),
                                              static_cast<float>(pos[1]),
                                              static_cast<float>(pos[2]), tp);
                    vert.px = tp[0] * scale;
                    vert.py = tp[1] * scale;
                    vert.pz = tp[2] * scale;

                    if (normals != nullptr)
                    {
                        cgltf_float nrm[3] = {0, 1, 0};
                        if (cgltf_accessor_read_float(normals, vi, nrm, 3))
                        {
                            float tn[3] = {0, 1, 0};
                            TransformNormal(ctx.world, static_cast<float>(nrm[0]),
                                                       static_cast<float>(nrm[1]),
                                                       static_cast<float>(nrm[2]), tn);
                            vert.nx = tn[0];
                            vert.ny = tn[1];
                            vert.nz = tn[2];
                        }
                    }
                    else
                    {
                        vert.ny = 1.0f;  // 与 OBJ 导入的缺省一致
                    }

                    if (texcoords != nullptr)
                    {
                        cgltf_float uv[2] = {0, 0};
                        if (cgltf_accessor_read_float(texcoords, vi, uv, 2))
                        {
                            vert.u = static_cast<float>(uv[0]);
                            vert.v = static_cast<float>(uv[1]);
                        }
                    }

                    // 与 OBJ 导入保持一致：切线没有源数据时填 (1,0,0)。
                    vert.tx = 1.0f;

                    out_mesh.vertex_buffer.push_back(vert);
                    out_mesh.index_buffer.push_back(
                        static_cast<int>(vertex_offset + out_mesh.vertex_buffer.size() - 1));
                }

                produced_triangles += index_count / 3;
            }
        }

        if (produced_triangles == 0)
        {
            out_error = "glTF contained no triangle geometry";
            return false;
        }

        return true;
    }
}
