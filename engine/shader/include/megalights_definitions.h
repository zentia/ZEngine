// Shared MegaLights constants (GLSL). Keep in sync with MegaLightsDefinitions.h (C++).

#ifndef MEGALIGHTS_DEFINITIONS_H_GLSL
#define MEGALIGHTS_DEFINITIONS_H_GLSL

#define ZENGINE_MEGALIGHTS_TILE_SIZE 8
#define ZENGINE_MEGALIGHTS_MAX_LIGHTS 1024
#define ZENGINE_MEGALIGHTS_MAX_LIGHTS_PER_TILE 64
#define ZENGINE_MEGALIGHTS_NUM_SAMPLES_DEFAULT 4

struct MegaLight
{
    highp vec4 position_radius;
    highp vec4 intensity;
};

#endif
