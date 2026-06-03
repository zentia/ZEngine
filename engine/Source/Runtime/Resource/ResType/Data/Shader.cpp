#include "Shader.h"

IMPLEMENT_REGISTER_CLASS(ShaderRes)
IMPLEMENT_OBJECT_SERAILIZE(ShaderRes)

template<typename TransferFunction>
void ShaderRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_ShaderName, "shader_name");
    transfer.Transfer(m_Properties, "properties");
    transfer.Transfer(m_Passes, "passes");

    transfer.Transfer(m_VertexShaderFile, "vertex_shader_file");
    transfer.Transfer(m_FragmentShaderFile, "fragment_shader_file");
    transfer.Transfer(m_RenderPipeline, "render_pipeline");
    transfer.Transfer(m_SourceLanguage, "source_language");
    transfer.Transfer(m_VertexEntry, "vertex_entry");
    transfer.Transfer(m_FragmentEntry, "fragment_entry");
    transfer.Transfer(m_IncludeDirectory, "include_directory");
    transfer.Transfer(m_EnableDx12, "enable_dx12");
    transfer.Transfer(m_EnableVulkan, "enable_vulkan");
    transfer.Transfer(m_EnableMetal, "enable_metal");
}

INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(ShaderRes)
