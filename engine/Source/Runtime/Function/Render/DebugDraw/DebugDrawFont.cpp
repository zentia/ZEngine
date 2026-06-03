#include "DebugDrawFont.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

void DebugDrawFont::LoadFont()
{
    std::string str = GET_SYSTEM(ConfigManager)->GetEditorFontPath().string();
    const char* fontFilePath = str.c_str();
    FILE* fontFile = fopen(fontFilePath, "rb");
    if (fontFile == NULL)
    {
        std::runtime_error("debug draw cannot open font.ttf");
    }
    fseek(fontFile, 0, SEEK_END);
    uint64_t size = ftell(fontFile);
    fseek(fontFile, 0, SEEK_SET);

    stbtt_fontinfo fontInfo;
    unsigned char* fontBuffer = (unsigned char*)calloc(size, sizeof(unsigned char));
    fread(fontBuffer, size, 1, fontFile);
    fclose(fontFile);

    if (!stbtt_InitFont(&fontInfo, fontBuffer, 0))
    {
        std::runtime_error("debug draw stb init font failed\n");
    }

    unsigned char* bitmap = (unsigned char*)calloc(m_BitmapW * m_BitmapH, sizeof(unsigned char));

    float pixels = m_Singlecharacterheight - 2;
    float scale = stbtt_ScaleForPixelHeight(&fontInfo, pixels);

    int c_x1, c_y1, c_x2, c_y2;
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
    ascent = roundf(ascent * scale);
    descent = roundf(descent * scale);

    int x = 0;
    for (unsigned char character = m_RangeL; character <= m_RangeR; character++)
    {
        int advanceWidth = 0;
        int leftSideBearing = 0;
        stbtt_GetCodepointHMetrics(&fontInfo, (unsigned char)character, &advanceWidth, &leftSideBearing);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(&fontInfo, character, scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int y = ascent + c_y1 - 2;
        int byteOffset =
            roundf(leftSideBearing * scale) +
            (character - m_RangeL) % m_Numofcharacterinoneline * m_Singlecharacterwidth +
            ((character - m_RangeL) / m_Numofcharacterinoneline * m_Singlecharacterheight + y) * m_BitmapW;

        stbtt_MakeCodepointBitmap(
            &fontInfo, bitmap + byteOffset, c_x2 - c_x1, c_y2 - c_y1, m_BitmapW, scale, scale, character);

        x += roundf(advanceWidth * scale);

        int kern;
        kern = stbtt_GetCodepointKernAdvance(&fontInfo, character, (unsigned char)(character + 1));
        x += roundf(kern * scale);
    }
    std::vector<float> imageData(m_BitmapW * m_BitmapH);
    for (int i = 0; i < m_BitmapW * m_BitmapH; i++)
    {
        imageData[i] = static_cast<float>(*(bitmap + i)) / 255.0f;
    }

    GET_SYSTEM(RHI)->CreateGlobalImage(m_FontImage,
                                       m_FontImageview,
                                       m_Allocation,
                                       m_BitmapW,
                                       m_BitmapH,
                                       imageData.data(),
                                       RHIFormat::RHI_FORMAT_R32_SFLOAT);

    free(fontBuffer);
    free(bitmap);
}

void DebugDrawFont::GetCharacterTextureRect(const unsigned char character, float& x1, float& y1, float& x2, float& y2)
{
    if (character >= m_RangeL && character <= m_RangeR)
    {
        x1 = (character - m_RangeL) % m_Numofcharacterinoneline * m_Singlecharacterwidth * 1.0f / m_BitmapW;
        x2 = ((character - m_RangeL) % m_Numofcharacterinoneline * m_Singlecharacterwidth + m_Singlecharacterwidth) *
             1.0f / m_BitmapW;
        y1 = (character - m_RangeL) / m_Numofcharacterinoneline * m_Singlecharacterheight * 1.0f / m_BitmapH;
        y2 = ((character - m_RangeL) / m_Numofcharacterinoneline * m_Singlecharacterheight + m_Singlecharacterheight) *
             1.0f / m_BitmapH;
    }
    else
    {
        x1 = x2 = y1 = y2 = 0;
    }
}

void DebugDrawFont::Inialize()
{
    LoadFont();
}

void DebugDrawFont::Destroy()
{
    GET_SYSTEM(RHI)->FreeMemory(m_FontImagememory);
    GET_SYSTEM(RHI)->DestroyImageView(m_FontImageview);
    GET_SYSTEM(RHI)->DestroyImage(m_FontImage);
}

RHIImageView* DebugDrawFont::GetImageView() const
{
    return m_FontImageview;
}