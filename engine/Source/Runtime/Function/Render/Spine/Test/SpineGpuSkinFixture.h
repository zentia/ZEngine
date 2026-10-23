#pragma once

// =====================================================================
// 换肤夹具（3.8 json）：OSGame 781 个资产**没有一个**带具名皮肤，换肤只能靠手写夹具验。
// SpineGpuImportTest（手算断言，进 ctest）与 SpineGpuRefParityTest（与 spine-cpp 随机脚本逐帧对拍）共用。
//
// 覆盖的换肤情形（Skeleton::setSkin / Skeleton::getAttachment / Skin::attachAll）：
//   s0 "body"      default = region、red = **同名不同类型**（rigid mesh，带 deform）、blue 没有 ⇒ 回落 default
//   s1 "eye"       default = rigid mesh（带 deform）、blue = 跨皮肤 linked mesh（parent = default/eye，
//                  deform: true ⇒ default 的 deform 时间轴作用在 blue 的附件上）；"eye_closed" default 与 red 各一份
//   s2 "hat"       **只有** red / blue 有：无皮肤时 setup 附件解析不到（= NULL），setSkin(red) 从 NULL 挂上
//   s3 "clip"      default / red 各一个裁剪附件（red 的更小、带 deform），end = s4
//   s4 "arm"       default = weighted mesh（带 deform）、blue = rigid mesh（带 deform）
//   s5 "track"     path 附件：default 与 blue 曲线不同 ⇒ path 约束 p1 摆出来的 b3 随皮肤变（骨骼可见差异），
//                  blue 的 path 还带 deform
// 骨骼：root(0) → b1(1) → b2(2)；root → b3(3)（被 p1 约束）。
// ⚠️ path 的 vertexCount 必须 ≥ 6（非闭合：曲线数 = vertexCount/3 - 1）：3 时参考实现读 lengths[-1] 断言。
// =====================================================================

namespace SpineGpuSkinFixture
{
    inline const char* kAtlas = "skins.png\n"
                                "size: 256,256\n"
                                "format: RGBA8888\n"
                                "filter: Linear,Linear\n"
                                "repeat: none\n"
                                "r1\n"
                                "  rotate: false\n"
                                "  xy: 0, 0\n"
                                "  size: 32, 32\n"
                                "r2\n"
                                "  rotate: false\n"
                                "  xy: 40, 0\n"
                                "  size: 32, 32\n"
                                "r3\n"
                                "  rotate: false\n"
                                "  xy: 80, 0\n"
                                "  size: 32, 16\n"
                                "r4\n"
                                "  rotate: true\n"
                                "  xy: 0, 40\n"
                                "  size: 48, 24\n";

    inline const char* kSkeleton = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [
    { "name": "root" },
    { "name": "b1", "parent": "root", "x": 10, "y": 5, "rotation": 15, "length": 30 },
    { "name": "b2", "parent": "b1", "x": 30, "rotation": -20, "length": 40 },
    { "name": "b3", "parent": "root", "x": -20, "y": -10, "length": 25 }
  ],
  "slots": [
    { "name": "s0", "bone": "b1", "attachment": "body" },
    { "name": "s1", "bone": "b2", "attachment": "eye" },
    { "name": "s2", "bone": "root", "attachment": "hat" },
    { "name": "s3", "bone": "b2", "attachment": "clip" },
    { "name": "s4", "bone": "b3", "attachment": "arm" },
    { "name": "s5", "bone": "root", "attachment": "track" }
  ],
  "path": [
    { "name": "p1", "order": 0, "bones": [ "b3" ], "target": "s5",
      "positionMode": "percent", "spacingMode": "length", "rotateMode": "tangent",
      "position": 0.5, "spacing": 0, "rotateMix": 1, "translateMix": 1 }
  ],
  "skins": {
    "default": {
      "s0": { "body": { "path": "r1", "x": 5, "y": 0, "width": 40, "height": 20, "rotation": 10 } },
      "s1": {
        "eye": { "type": "mesh", "path": "r2",
                 "uvs": [ 0,0, 1,0, 1,1, 0,1 ], "triangles": [ 0,1,2, 2,3,0 ],
                 "vertices": [ -5,-5, 5,-5, 5,5, -5,5 ], "hull": 4 },
        "eye_closed": { "path": "r3", "width": 8, "height": 2 }
      },
      "s3": { "clip": { "type": "clipping", "end": "s4", "vertexCount": 4,
                        "vertices": [ -40,-40, 60,-40, 60,50, -40,50 ] } },
      "s4": { "arm": { "type": "mesh", "path": "r2",
                       "uvs": [ 0,0, 1,0, 1,1, 0,1 ], "triangles": [ 0,1,2, 2,3,0 ],
                       "vertices": [ 1, 3,-6,-6,1,
                                     2, 3,6,-6,0.5, 2,4,-8,0.5,
                                     1, 3,6,6,1,
                                     1, 2,-6,6,1 ], "hull": 4 } },
      "s5": { "track": { "type": "path", "closed": false, "constantSpeed": false,
                         "vertexCount": 6, "vertices": [ -10,0, 0,0, 10,0,  50,10, 60,20, 70,30 ],
                         "lengths": [ 35, 70 ] } }
    },
    "red": {
      "s0": { "body": { "type": "mesh", "path": "r4",
                        "uvs": [ 0,0, 1,0, 1,1, 0,1 ], "triangles": [ 0,1,2, 2,3,0 ],
                        "vertices": [ -12,-4, 14,-6, 16,8, -10,6 ], "hull": 4 } },
      "s1": { "eye_closed": { "path": "r1", "width": 12, "height": 3 } },
      "s2": { "hat": { "path": "r3", "x": 0, "y": 40, "width": 20, "height": 10 } },
      "s3": { "clip": { "type": "clipping", "end": "s4", "vertexCount": 4,
                        "vertices": [ -10,-10, 20,-10, 20,15, -10,15 ] } }
    },
    "blue": {
      "s1": { "eye": { "type": "linkedmesh", "path": "r1", "parent": "eye", "skin": "default", "deform": true } },
      "s2": { "hat": { "path": "r1", "x": 3, "y": 30, "width": 6, "height": 6 } },
      "s4": { "arm": { "type": "mesh", "path": "r2",
                       "uvs": [ 0,0, 1,0, 1,1, 0,1 ], "triangles": [ 0,1,2, 2,3,0 ],
                       "vertices": [ -3,-9, 9,-3, 3,9, -9,3 ], "hull": 4 } },
      "s5": { "track": { "type": "path", "closed": false, "constantSpeed": false,
                         "vertexCount": 6, "vertices": [ 10,0, 0,0, -10,5,  -50,-10, -60,-20, -70,-30 ],
                         "lengths": [ 35, 70 ] } }
    }
  },
  "animations": {
    "blink": {
      "slots": {
        "s0": { "color": [ { "time": 0, "color": "ffffffff" }, { "time": 0.7, "color": "ff8040c0" } ] },
        "s1": { "attachment": [ { "time": 0, "name": "eye" }, { "time": 0.3, "name": "eye_closed" },
                                { "time": 0.6, "name": "eye" } ] },
        "s2": { "attachment": [ { "time": 0.2, "name": null }, { "time": 0.5, "name": "hat" } ] }
      },
      "bones": {
        "b1": { "rotate": [ { "time": 0, "angle": 0 }, { "time": 1, "angle": 40 } ] }
      },
      "deform": {
        "default": {
          "s1": { "eye": [ { "time": 0 }, { "time": 0.5, "offset": 0, "vertices": [ 1,1, 2,-2, 3,3, -4,4 ] },
                           { "time": 1 } ] },
          "s4": { "arm": [ { "time": 0, "offset": 2, "vertices": [ 1,2, 3,4 ] }, { "time": 0.9 } ] }
        },
        "red": {
          "s0": { "body": [ { "time": 0, "offset": 0, "vertices": [ -14,-5, 15,-7, 17,9, -11,7 ] },
                            { "time": 0.8, "offset": 0, "vertices": [ -12,-4, 14,-6, 16,8, -10,6 ] } ] },
          "s3": { "clip": [ { "time": 0.1, "offset": 0, "vertices": [ -12,-12, 22,-12, 22,17, -12,17 ] },
                            { "time": 0.6 } ] }
        },
        "blue": {
          "s4": { "arm": [ { "time": 0.2, "offset": 0, "vertices": [ -4,-10, 10,-4, 4,10, -10,4 ] } ] },
          "s5": { "track": [ { "time": 0, "offset": 2, "vertices": [ -32,14 ] }, { "time": 0.7 } ] }
        }
      }
    },
    "wave": {
      "bones": {
        "b2": { "rotate": [ { "time": 0, "angle": -30 }, { "time": 0.5, "angle": 30 }, { "time": 1, "angle": -30 } ] },
        "b3": { "translate": [ { "time": 0, "x": 0, "y": 0 }, { "time": 1, "x": 10, "y": -5 } ] }
      },
      "slots": {
        "s0": { "attachment": [ { "time": 0.4, "name": "body" } ] },
        "s2": { "attachment": [ { "time": 0.1, "name": "hat" } ] }
      },
      "drawOrder": [ { "time": 0.4, "offsets": [ { "slot": "s3", "offset": -2 } ] }, { "time": 0.9 } ]
    }
  }
})";
}   // namespace SpineGpuSkinFixture
