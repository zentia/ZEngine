#pragma once
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Level/Level.h"

class LevelDebugger
{
public:
    void Tick(Level* level) const;

    // show all bones in a level
    void ShowAllBones(Level* level) const;
    // show all bones of a object
    void ShowBones(Level* level, GObjectID go_id) const;
    // show all bones' name in a level
    void ShowAllBonesName(Level* level) const;
    // show all bones' name of a object
    void ShowBonesName(Level* level, GObjectID go_id) const;
    // show all bindingBox in a level
    void ShowAllBoundingBox(Level* level) const;
    // show boundingBox of a object
    void ShowBoundingBox(Level* level, GObjectID go_id) const;
    // show camera info
    void ShowCameraInfo(Level* level) const;

private:
    void DrawBones(std::shared_ptr<GameObject> object) const;
    void DrawBonesName(std::shared_ptr<GameObject> object) const;
    void DrawBoundingBox(std::shared_ptr<GameObject> object) const;
    void DrawCameraInfo(std::shared_ptr<GameObject> object) const;
};