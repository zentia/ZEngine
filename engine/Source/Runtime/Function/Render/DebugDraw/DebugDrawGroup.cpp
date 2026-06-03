#include "DebugDrawGroup.h"

#include "Runtime/Function/Render/RenderSystem.h"

#include <vector>

DebugDrawGroup::~DebugDrawGroup()
{
    clear();
}
void DebugDrawGroup::Initialize() {}

void DebugDrawGroup::clear()
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    ClearData();
}

void DebugDrawGroup::ClearData()
{
    m_Points.clear();
    m_Lines.clear();
    m_Triangles.clear();
    m_Quads.clear();
    m_Boxes.clear();
    m_Cylinders.clear();
    m_Spheres.clear();
    m_Capsules.clear();
    m_Texts.clear();
}

void DebugDrawGroup::SetName(const std::string& name)
{
    m_Name = name;
}

const std::string& DebugDrawGroup::GetName() const
{
    return m_Name;
}

void DebugDrawGroup::AddPoint(const Vector3& position,
                              const Vector4& color,
                              const float life_time,
                              const bool no_depth_test)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawPoint point;
    point.m_Vertex.color = color;
    point.SetTime(life_time);
    point.m_FillMode = _FillMode_wireframe;
    point.m_Vertex.pos = position;
    point.m_NoDepthTest = no_depth_test;
    m_Points.push_back(point);
}

void DebugDrawGroup::AddLine(const Vector3& point0,
                             const Vector3& point1,
                             const Vector4& color0,
                             const Vector4& color1,
                             const float life_time,
                             const bool no_depth_test)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawLine line;
    line.SetTime(life_time);
    line.m_FillMode = _FillMode_wireframe;
    line.m_NoDepthTest = no_depth_test;

    line.m_Vertex[0].pos = point0;
    line.m_Vertex[0].color = color0;

    line.m_Vertex[1].pos = point1;
    line.m_Vertex[1].color = color1;

    m_Lines.push_back(line);
}

void DebugDrawGroup::AddTriangle(const Vector3& point0,
                                 const Vector3& point1,
                                 const Vector3& point2,
                                 const Vector4& color0,
                                 const Vector4& color1,
                                 const Vector4& color2,
                                 const float life_time,
                                 const bool no_depth_test,
                                 const FillMode fillmod)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawTriangle triangle;
    triangle.SetTime(life_time);
    triangle.m_FillMode = fillmod;
    triangle.m_NoDepthTest = no_depth_test;

    triangle.m_Vertex[0].pos = point0;
    triangle.m_Vertex[0].color = color0;

    triangle.m_Vertex[1].pos = point1;
    triangle.m_Vertex[1].color = color1;

    triangle.m_Vertex[2].pos = point2;
    triangle.m_Vertex[2].color = color2;

    m_Triangles.push_back(triangle);
}

void DebugDrawGroup::AddQuad(const Vector3& point0,
                             const Vector3& point1,
                             const Vector3& point2,
                             const Vector3& point3,
                             const Vector4& color0,
                             const Vector4& color1,
                             const Vector4& color2,
                             const Vector4& color3,
                             const float life_time,
                             const bool no_depth_test,
                             const FillMode fillmode)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    if (fillmode == _FillMode_wireframe)
    {
        DebugDrawQuad quad;

        quad.m_Vertex[0].pos = point0;
        quad.m_Vertex[0].color = color0;

        quad.m_Vertex[1].pos = point1;
        quad.m_Vertex[1].color = color1;

        quad.m_Vertex[2].pos = point2;
        quad.m_Vertex[2].color = color2;

        quad.m_Vertex[3].pos = point3;
        quad.m_Vertex[3].color = color3;

        quad.SetTime(life_time);
        quad.m_NoDepthTest = no_depth_test;

        m_Quads.push_back(quad);
    }
    else
    {
        DebugDrawTriangle triangle;
        triangle.SetTime(life_time);
        triangle.m_FillMode = _FillMode_solid;
        triangle.m_NoDepthTest = no_depth_test;

        triangle.m_Vertex[0].pos = point0;
        triangle.m_Vertex[0].color = color0;
        triangle.m_Vertex[1].pos = point1;
        triangle.m_Vertex[1].color = color1;
        triangle.m_Vertex[2].pos = point2;
        triangle.m_Vertex[2].color = color2;
        m_Triangles.push_back(triangle);

        triangle.m_Vertex[0].pos = point0;
        triangle.m_Vertex[0].color = color0;
        triangle.m_Vertex[1].pos = point2;
        triangle.m_Vertex[1].color = color2;
        triangle.m_Vertex[2].pos = point3;
        triangle.m_Vertex[2].color = color3;
        m_Triangles.push_back(triangle);
    }
}

void DebugDrawGroup::AddBox(const Vector3& center_point,
                            const Vector3& half_extends,
                            const Vector4& rotate,
                            const Vector4& color,
                            const float life_time,
                            const bool no_depth_test)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawBox box;
    box.m_CenterPoint = center_point;
    box.m_HalfExtents = half_extends;
    box.m_Rotate = rotate;
    box.m_Color = color;
    box.m_NoDepthTest = no_depth_test;
    box.SetTime(life_time);

    m_Boxes.push_back(box);
}

void DebugDrawGroup::AddSphere(const Vector3& center,
                               const float radius,
                               const Vector4& color,
                               const float life_time,
                               const bool no_depth_test)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawSphere sphere;
    sphere.m_Center = center;
    sphere.m_Radius = radius;
    sphere.m_Color = color;
    sphere.m_NoDepthTest = no_depth_test;
    sphere.SetTime(life_time);

    m_Spheres.push_back(sphere);
}

void DebugDrawGroup::AddCylinder(const Vector3& center,
                                 const float radius,
                                 const float height,
                                 const Vector4& rotate,
                                 const Vector4& color,
                                 const float life_time,
                                 const bool no_depth_test)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawCylinder cylinder;
    cylinder.m_Radius = radius;
    cylinder.m_Center = center;
    cylinder.m_Height = height;
    cylinder.m_Rotate = rotate;
    cylinder.m_Color = color;
    cylinder.m_NoDepthTest = no_depth_test;
    cylinder.SetTime(life_time);

    m_Cylinders.push_back(cylinder);
}

void DebugDrawGroup::AddCapsule(const Vector3& center,
                                const Vector4& rotation,
                                const Vector3& scale,
                                const float radius,
                                const float height,
                                const Vector4& color,
                                const float life_time,
                                const bool no_depth_test)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawCapsule capsule;
    capsule.m_Center = center;
    capsule.m_Rotation = rotation;
    capsule.m_Scale = scale;
    capsule.m_Radius = radius;
    capsule.m_Height = height;
    capsule.m_Color = color;
    capsule.m_NoDepthTest = no_depth_test;
    capsule.SetTime(life_time);

    m_Capsules.push_back(capsule);
}

void DebugDrawGroup::AddText(const std::string& content,
                             const Vector4& color,
                             const Vector3& coordinate,
                             const int size,
                             const bool is_screen_text,
                             const float life_time)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    DebugDrawText text;
    text.m_Content = content;
    text.m_Color = color;
    text.m_Coordinate = coordinate;
    text.m_Size = size;
    text.m_IsScreenText = is_screen_text;
    text.SetTime(life_time);
    m_Texts.push_back(text);
}

void DebugDrawGroup::RemoveDeadPrimitives(float delta_time)
{
    for (std::list<DebugDrawPoint>::iterator point = m_Points.begin(); point != m_Points.end();)
    {
        if (point->IsTimeOut(delta_time))
        {
            point = m_Points.erase(point);
        }
        else
        {
            point++;
        }
    }
    for (std::list<DebugDrawLine>::iterator line = m_Lines.begin(); line != m_Lines.end();)
    {
        if (line->IsTimeOut(delta_time))
        {
            line = m_Lines.erase(line);
        }
        else
        {
            line++;
        }
    }
    for (std::list<DebugDrawTriangle>::iterator triangle = m_Triangles.begin(); triangle != m_Triangles.end();)
    {
        if (triangle->IsTimeOut(delta_time))
        {
            triangle = m_Triangles.erase(triangle);
        }
        else
        {
            triangle++;
        }
    }
    for (std::list<DebugDrawQuad>::iterator quad = m_Quads.begin(); quad != m_Quads.end();)
    {
        if (quad->IsTimeOut(delta_time))
        {
            quad = m_Quads.erase(quad);
        }
        else
        {
            quad++;
        }
    }
    for (std::list<DebugDrawBox>::iterator box = m_Boxes.begin(); box != m_Boxes.end();)
    {
        if (box->IsTimeOut(delta_time))
        {
            box = m_Boxes.erase(box);
        }
        else
        {
            box++;
        }
    }
    for (std::list<DebugDrawCylinder>::iterator cylinder = m_Cylinders.begin(); cylinder != m_Cylinders.end();)
    {
        if (cylinder->IsTimeOut(delta_time))
        {
            cylinder = m_Cylinders.erase(cylinder);
        }
        else
        {
            cylinder++;
        }
    }
    for (std::list<DebugDrawSphere>::iterator sphere = m_Spheres.begin(); sphere != m_Spheres.end();)
    {
        if (sphere->IsTimeOut(delta_time))
        {
            sphere = m_Spheres.erase(sphere);
        }
        else
        {
            sphere++;
        }
    }
    for (std::list<DebugDrawCapsule>::iterator capsule = m_Capsules.begin(); capsule != m_Capsules.end();)
    {
        if (capsule->IsTimeOut(delta_time))
        {
            capsule = m_Capsules.erase(capsule);
        }
        else
        {
            capsule++;
        }
    }
    for (std::list<DebugDrawText>::iterator text = m_Texts.begin(); text != m_Texts.end();)
    {
        if (text->IsTimeOut(delta_time))
        {
            text = m_Texts.erase(text);
        }
        else
        {
            text++;
        }
    }
}

void DebugDrawGroup::MergeFrom(DebugDrawGroup* group)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    std::lock_guard<std::mutex> guard_2(group->m_Mutex);
    m_Points.insert(m_Points.end(), group->m_Points.begin(), group->m_Points.end());
    m_Lines.insert(m_Lines.end(), group->m_Lines.begin(), group->m_Lines.end());
    m_Triangles.insert(m_Triangles.end(), group->m_Triangles.begin(), group->m_Triangles.end());
    m_Quads.insert(m_Quads.end(), group->m_Quads.begin(), group->m_Quads.end());
    m_Boxes.insert(m_Boxes.end(), group->m_Boxes.begin(), group->m_Boxes.end());
    m_Cylinders.insert(m_Cylinders.end(), group->m_Cylinders.begin(), group->m_Cylinders.end());
    m_Spheres.insert(m_Spheres.end(), group->m_Spheres.begin(), group->m_Spheres.end());
    m_Capsules.insert(m_Capsules.end(), group->m_Capsules.begin(), group->m_Capsules.end());
    m_Texts.insert(m_Texts.end(), group->m_Texts.begin(), group->m_Texts.end());
}

size_t DebugDrawGroup::GetPointCount(bool no_depth_test) const
{
    size_t count = 0;
    for (const DebugDrawPoint point : m_Points)
    {
        if (point.m_NoDepthTest == no_depth_test)
            count++;
    }
    return count;
}

size_t DebugDrawGroup::GetLineCount(bool no_depth_test) const
{
    size_t line_count = 0;
    for (const DebugDrawLine line : m_Lines)
    {
        if (line.m_NoDepthTest == no_depth_test)
            line_count++;
    }
    for (const DebugDrawTriangle triangle : m_Triangles)
    {
        if (triangle.m_FillMode == FillMode::_FillMode_wireframe && triangle.m_NoDepthTest == no_depth_test)
        {
            line_count += 3;
        }
    }
    for (const DebugDrawQuad quad : m_Quads)
    {
        if (quad.m_FillMode == FillMode::_FillMode_wireframe && quad.m_NoDepthTest == no_depth_test)
        {
            line_count += 4;
        }
    }
    for (const DebugDrawBox box : m_Boxes)
    {
        if (box.m_NoDepthTest == no_depth_test)
            line_count += 12;
    }
    return line_count;
}

size_t DebugDrawGroup::GetTriangleCount(bool no_depth_test) const
{
    size_t triangle_count = 0;
    for (const DebugDrawTriangle triangle : m_Triangles)
    {
        if (triangle.m_FillMode == FillMode::_FillMode_solid && triangle.m_NoDepthTest == no_depth_test)
        {
            triangle_count++;
        }
    }
    return triangle_count;
}

size_t DebugDrawGroup::GetUniformDynamicDataCount() const
{
    return m_Spheres.size() + m_Cylinders.size() + m_Capsules.size();
}

void DebugDrawGroup::WritePointData(std::vector<DebugDrawVertex>& vertexs, bool no_depth_test)
{
    size_t vertexs_count = GetPointCount(no_depth_test);
    vertexs.resize(vertexs_count);

    size_t current_index = 0;
    for (DebugDrawPoint point : m_Points)
    {
        if (point.m_NoDepthTest == no_depth_test)
            vertexs[current_index++] = point.m_Vertex;
    }
}

void DebugDrawGroup::WriteLineData(std::vector<DebugDrawVertex>& vertexs, bool no_depth_test)
{
    size_t vertexs_count = GetLineCount(no_depth_test) * 2;
    vertexs.resize(vertexs_count);

    size_t current_index = 0;
    for (DebugDrawLine line : m_Lines)
    {
        if (line.m_FillMode == FillMode::_FillMode_wireframe && line.m_NoDepthTest == no_depth_test)
        {
            vertexs[current_index++] = line.m_Vertex[0];
            vertexs[current_index++] = line.m_Vertex[1];
        }
    }
    for (DebugDrawTriangle triangle : m_Triangles)
    {
        if (triangle.m_FillMode == FillMode::_FillMode_wireframe && triangle.m_NoDepthTest == no_depth_test)
        {
            std::vector<size_t> indies = {0, 1, 1, 2, 2, 0};
            for (size_t i : indies)
            {
                vertexs[current_index++] = triangle.m_Vertex[i];
            }
        }
    }
    for (DebugDrawQuad quad : m_Quads)
    {
        if (quad.m_FillMode == FillMode::_FillMode_wireframe && quad.m_NoDepthTest == no_depth_test)
        {
            std::vector<size_t> indies = {0, 1, 1, 2, 2, 3, 3, 0};
            for (size_t i : indies)
            {
                vertexs[current_index++] = quad.m_Vertex[i];
            }
        }
    }
    for (DebugDrawBox box : m_Boxes)
    {
        if (box.m_NoDepthTest == no_depth_test)
        {
            std::vector<DebugDrawVertex> verts_4d(8);
            float f[2] = {-1.0f, 1.0f};
            for (size_t i = 0; i < 8; i++)
            {
                Vector3 v(f[i & 1] * box.m_HalfExtents.x,
                          f[(i >> 1) & 1] * box.m_HalfExtents.y,
                          f[(i >> 2) & 1] * box.m_HalfExtents.z);
                Vector3 uv, uuv;
                Vector3 qvec(box.m_Rotate.x, box.m_Rotate.y, box.m_Rotate.z);
                uv = qvec.crossProduct(v);
                uuv = qvec.crossProduct(uv);
                uv *= (2.0f * box.m_Rotate.w);
                uuv *= 2.0f;
                verts_4d[i].pos = v + uv + uuv + box.m_CenterPoint;
                verts_4d[i].color = box.m_Color;
            }
            std::vector<size_t> indies = {0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7, 7, 6, 6, 4, 0, 4, 1, 5, 3, 7, 2, 6};
            for (size_t i : indies)
            {
                vertexs[current_index++] = verts_4d[i];
            }
        }
    }
}

void DebugDrawGroup::WriteTriangleData(std::vector<DebugDrawVertex>& vertexs, bool no_depth_test)
{
    size_t vertexs_count = GetTriangleCount(no_depth_test) * 3;
    vertexs.resize(vertexs_count);

    size_t current_index = 0;
    for (DebugDrawTriangle triangle : m_Triangles)
    {
        if (triangle.m_FillMode == FillMode::_FillMode_solid && triangle.m_NoDepthTest == no_depth_test)
        {
            vertexs[current_index++] = triangle.m_Vertex[0];
            vertexs[current_index++] = triangle.m_Vertex[1];
            vertexs[current_index++] = triangle.m_Vertex[2];
        }
    }
}

void DebugDrawGroup::WriteTextData(std::vector<DebugDrawVertex>& vertexs,
                                   DebugDrawFont* font,
                                   Matrix4x4 m_ProjViewMatrix)
{
    RHISwapChainDesc swapChainDesc = GET_SYSTEM(RHI)->GetSwapchainInfo();
    uint32_t screenWidth = swapChainDesc.viewport->width;
    uint32_t screenHeight = swapChainDesc.viewport->height;

    size_t vertexs_count = GetTextCharacterCount() * 6;
    vertexs.resize(vertexs_count);

    size_t current_index = 0;
    for (DebugDrawText text : m_Texts)
    {
        float absoluteW = text.m_Size, absoluteH = text.m_Size * 2;
        float w = absoluteW / (1.0f * screenWidth / 2.0f), h = absoluteH / (1.0f * screenHeight / 2.0f);
        Vector3 coordinate = text.m_Coordinate;
        if (!text.m_IsScreenText)
        {
            Vector4 tempCoord(coordinate.x, coordinate.y, coordinate.z, 1.0f);
            tempCoord = m_ProjViewMatrix * tempCoord;
            coordinate = Vector3(tempCoord.x / tempCoord.w, tempCoord.y / tempCoord.w, 0.0f);
        }
        float x = coordinate.x, y = coordinate.y;
        for (unsigned char character : text.m_Content)
        {
            if (character == '\n')
            {
                y += h;
                x = coordinate.x;
            }
            else
            {
                float x1, x2, y1, y2;
                font->GetCharacterTextureRect(character, x1, y1, x2, y2);

                float cx1, cx2, cy1, cy2;
                cx1 = 0 + x;
                cx2 = w + x;
                cy1 = 0 + y;
                cy2 = h + y;

                vertexs[current_index].pos = Vector3(cx1, cy1, 0.0f);
                vertexs[current_index].color = text.m_Color;
                vertexs[current_index++].texcoord = Vector2(x1, y1);

                vertexs[current_index].pos = Vector3(cx1, cy2, 0.0f);
                vertexs[current_index].color = text.m_Color;
                vertexs[current_index++].texcoord = Vector2(x1, y2);

                vertexs[current_index].pos = Vector3(cx2, cy2, 0.0f);
                vertexs[current_index].color = text.m_Color;
                vertexs[current_index++].texcoord = Vector2(x2, y2);

                vertexs[current_index].pos = Vector3(cx1, cy1, 0.0f);
                vertexs[current_index].color = text.m_Color;
                vertexs[current_index++].texcoord = Vector2(x1, y1);

                vertexs[current_index].pos = Vector3(cx2, cy2, 0.0f);
                vertexs[current_index].color = text.m_Color;
                vertexs[current_index++].texcoord = Vector2(x2, y2);

                vertexs[current_index].pos = Vector3(cx2, cy1, 0.0f);
                vertexs[current_index].color = text.m_Color;
                vertexs[current_index++].texcoord = Vector2(x2, y1);

                x += w;
            }
        }
    }
}

void DebugDrawGroup::WriteUniformDynamicDataToCache(std::vector<std::pair<Matrix4x4, Vector4>>& datas)
{
    // cache uniformDynamic data ,first has_depth_test ,second no_depth_test
    size_t data_count = GetUniformDynamicDataCount() * 3;
    datas.resize(data_count);

    bool no_depth_tests[] = {false, true};
    for (int32_t i = 0; i < 2; i++)
    {
        bool no_depth_test = no_depth_tests[i];

        size_t current_index = 0;
        for (DebugDrawSphere sphere : m_Spheres)
        {
            if (sphere.m_NoDepthTest == no_depth_test)
            {
                Matrix4x4 model = Matrix4x4::IDENTITY;

                Matrix4x4 tmp = Matrix4x4::IDENTITY;
                tmp.makeTrans(sphere.m_Center);
                model = model * tmp;
                tmp = Matrix4x4::BuildScaleMatrix(sphere.m_Radius, sphere.m_Radius, sphere.m_Radius);
                model = model * tmp;
                datas[current_index++] = std::make_pair(model, sphere.m_Color);
            }
        }
        for (DebugDrawCylinder cylinder : m_Cylinders)
        {
            if (cylinder.m_NoDepthTest == no_depth_test)
            {
                Matrix4x4 model = Matrix4x4::IDENTITY;

                // rolate
                float w = cylinder.m_Rotate.x;
                float x = cylinder.m_Rotate.y;
                float y = cylinder.m_Rotate.z;
                float z = cylinder.m_Rotate.w;
                Matrix4x4 tmp = Matrix4x4::IDENTITY;
                tmp.makeTrans(cylinder.m_Center);
                model = model * tmp;

                tmp = Matrix4x4::BuildScaleMatrix(cylinder.m_Radius, cylinder.m_Radius, cylinder.m_Height / 2.0f);
                model = model * tmp;

                Matrix4x4 ro = Matrix4x4::IDENTITY;
                ro[0][0] = 1.0f - 2.0f * y * y - 2.0f * z * z;
                ro[0][1] = 2.0f * x * y + 2.0f * w * z;
                ro[0][2] = 2.0f * x * z - 2.0f * w * y;
                ro[1][0] = 2.0f * x * y - 2.0f * w * z;
                ro[1][1] = 1.0f - 2.0f * x * x - 2.0f * z * z;
                ro[1][2] = 2.0f * y * z + 2.0f * w * x;
                ro[2][0] = 2.0f * x * z + 2.0f * w * y;
                ro[2][1] = 2.0f * y * z - 2.0f * w * x;
                ro[2][2] = 1.0f - 2.0f * x * x - 2.0f * y * y;
                model = model * ro;

                datas[current_index++] = std::make_pair(model, cylinder.m_Color);
            }
        }
        for (DebugDrawCapsule capsule : m_Capsules)
        {
            if (capsule.m_NoDepthTest == no_depth_test)
            {
                Matrix4x4 model1 = Matrix4x4::IDENTITY;
                Matrix4x4 model2 = Matrix4x4::IDENTITY;
                Matrix4x4 model3 = Matrix4x4::IDENTITY;

                Matrix4x4 tmp = Matrix4x4::IDENTITY;
                tmp.makeTrans(capsule.m_Center);
                model1 = model1 * tmp;
                model2 = model2 * tmp;
                model3 = model3 * tmp;

                tmp = Matrix4x4::BuildScaleMatrix(capsule.m_Scale.x, capsule.m_Scale.y, capsule.m_Scale.z);
                model1 = model1 * tmp;
                model2 = model2 * tmp;
                model3 = model3 * tmp;

                // rolate
                float w = capsule.m_Rotation.x;
                float x = capsule.m_Rotation.y;
                float y = capsule.m_Rotation.z;
                float z = capsule.m_Rotation.w;
                Matrix4x4 ro = Matrix4x4::IDENTITY;
                ro[0][0] = 1.0f - 2.0f * y * y - 2.0f * z * z;
                ro[0][1] = 2.0f * x * y + 2.0f * w * z;
                ro[0][2] = 2.0f * x * z - 2.0f * w * y;
                ro[1][0] = 2.0f * x * y - 2.0f * w * z;
                ro[1][1] = 1.0f - 2.0f * x * x - 2.0f * z * z;
                ro[1][2] = 2.0f * y * z + 2.0f * w * x;
                ro[2][0] = 2.0f * x * z + 2.0f * w * y;
                ro[2][1] = 2.0f * y * z - 2.0f * w * x;
                ro[2][2] = 1.0f - 2.0f * x * x - 2.0f * y * y;
                model1 = model1 * ro;
                model2 = model2 * ro;
                model3 = model3 * ro;

                tmp.makeTrans(Vector3(0.0f, 0.0f, capsule.m_Height / 2.0f - capsule.m_Radius));
                model1 = model1 * tmp;

                tmp = Matrix4x4::BuildScaleMatrix(1.0f, 1.0f, capsule.m_Height / (capsule.m_Radius * 2.0f));
                model2 = model2 * tmp;

                tmp.makeTrans(Vector3(0.0f, 0.0f, -(capsule.m_Height / 2.0f - capsule.m_Radius)));
                model3 = model3 * tmp;

                tmp = Matrix4x4::BuildScaleMatrix(capsule.m_Radius, capsule.m_Radius, capsule.m_Radius);
                model1 = model1 * tmp;
                model2 = model2 * tmp;
                model3 = model3 * tmp;

                datas[current_index++] = std::make_pair(model1, capsule.m_Color);
                datas[current_index++] = std::make_pair(model2, capsule.m_Color);
                datas[current_index++] = std::make_pair(model3, capsule.m_Color);
            }
        }
    }
}

size_t DebugDrawGroup::GetSphereCount(bool no_depth_test) const
{
    size_t count = 0;
    for (const DebugDrawSphere sphere : m_Spheres)
    {
        if (sphere.m_NoDepthTest == no_depth_test)
            count++;
    }
    return count;
}
size_t DebugDrawGroup::GetCylinderCount(bool no_depth_test) const
{
    size_t count = 0;
    for (const DebugDrawCylinder cylinder : m_Cylinders)
    {
        if (cylinder.m_NoDepthTest == no_depth_test)
            count++;
    }
    return count;
}
size_t DebugDrawGroup::GetCapsuleCount(bool no_depth_test) const
{
    size_t count = 0;
    for (const DebugDrawCapsule capsule : m_Capsules)
    {
        if (capsule.m_NoDepthTest == no_depth_test)
            count++;
    }
    return count;
}
size_t DebugDrawGroup::GetTextCharacterCount() const
{
    size_t count = 0;
    for (const DebugDrawText text : m_Texts)
    {
        for (unsigned char character : text.m_Content)
        {
            if (character != '\n')
                count++;
        }
    }
    return count;
}