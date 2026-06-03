#pragma once

namespace MegaLights
{

bool IsEnabled();
bool IsTemporalDenoiseEnabled();
bool IsSpatialDenoiseEnabled();
int GetNumSamplesPerPixel();
int GetScreenSpaceShadowSteps();
int GetMaxLights();
int GetSpatialRadius();
float GetTemporalBlend();
float GetDisocclusionThreshold();
float GetSpatialDepthSigma();
float GetSpatialNormalPower();

void SetEnabled(bool enabled);
void RegisterConsoleVariables();

}  // namespace MegaLights
