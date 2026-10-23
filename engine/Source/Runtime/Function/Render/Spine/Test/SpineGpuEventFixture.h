#pragma once

// =====================================================================
// 用户事件夹具（3.8 json，v16）：OSGame 782 个资产**没有一个**带用户事件，事件只能靠手写夹具 +
// spine-runtimes 官方示例（I:/spine-runtimes/examples，spineboy / hero / alien）验。
// SpineGpuImportTest（手算断言 + --dump 给 C# 对拍）与 SpineGpuRefParityTest（与 spine-cpp 随机脚本逐帧对拍）共用。
//
// 覆盖的情形（EventTimeline::apply / AnimationState::queueEvents / SkeletonJson 的事件读法）：
//   定义  step        int / float / string 都有 ⇒ 帧上缺省时继承
//         sfx         带 audio ⇒ 帧上逐帧读 volume / balance（缺省 1 / 0，**不**继承定义的 0.8 / -0.25）
//         blank       什么都没写 ⇒ 0 / 0 / "" / 无音频
//         mute        `"audio": ""` ⇒ 定义那层照读 volume（参考实现判的是指针非空），但帧那层判
//                     `getAudioPath().isEmpty()` ⇒ 帧上的 volume / balance **不读**（保持 1 / 0）
//   动画  walk  1.0 s：t=0（首帧 lastTime = -1 就触发）、t=0.5 两个同时刻（binarySearch 后往前退）、
//                     t=1.0（= animationEnd：循环回绕时排在 complete 之前）、帧上覆盖 int / float / string
//         late  骨骼时间轴只到 0.5，事件在 0.8 ⇒ duration = 0.8（v15 少算成 0.5）
//         solo  只有一条 1 帧的事件时间轴（t=0.25）⇒ duration = 0.25；1 帧走"lastTime ≥ 末帧"分支
//         burst 0 / 0 / 0.3 / 0.3 / 0.3 ⇒ 同时刻多事件在首帧与中段各一次
// 骨骼：root(0) → b1(1)。没有附件（事件与绘制无关；也省得依赖某份图集）。
// =====================================================================

namespace SpineGpuEventFixture
{
    inline const char* kAtlas = "events.png\n"
                                "size: 16,16\n"
                                "format: RGBA8888\n"
                                "filter: Linear,Linear\n"
                                "repeat: none\n"
                                "r1\n"
                                "  rotate: false\n"
                                "  xy: 0, 0\n"
                                "  size: 8, 8\n";

    inline const char* kSkeleton = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [
    { "name": "root" },
    { "name": "b1", "parent": "root", "x": 10, "y": 5, "rotation": 15, "length": 30 }
  ],
  "slots": [
    { "name": "s0", "bone": "b1" }
  ],
  "events": {
    "step": { "int": 7, "float": 0.5, "string": "left" },
    "sfx": { "audio": "boom.wav", "volume": 0.8, "balance": -0.25 },
    "blank": {},
    "mute": { "audio": "", "volume": 0.3, "balance": 0.6 }
  },
  "animations": {
    "walk": {
      "bones": {
        "b1": {
          "rotate": [ { "time": 0, "angle": 0 }, { "time": 0.5, "angle": 30 }, { "time": 1.0, "angle": 0 } ],
          "translate": [ { "time": 0, "x": 0, "y": 0 }, { "time": 1.0, "x": 12, "y": -4 } ]
        }
      },
      "events": [
        { "time": 0, "name": "step" },
        { "time": 0.5, "name": "step", "int": -3, "string": "right" },
        { "time": 0.5, "name": "sfx", "volume": 0.4, "balance": 0.75 },
        { "time": 1.0, "name": "blank", "float": 2.25 }
      ]
    },
    "late": {
      "bones": {
        "b1": { "rotate": [ { "time": 0, "angle": -10 }, { "time": 0.5, "angle": 10 } ] }
      },
      "events": [
        { "time": 0.2, "name": "sfx" },
        { "time": 0.8, "name": "mute", "volume": 0.1, "balance": 0.2 }
      ]
    },
    "solo": {
      "events": [ { "time": 0.25, "name": "step", "float": -1.5 } ]
    },
    "burst": {
      "bones": {
        "b1": { "translate": [ { "time": 0, "x": 0 }, { "time": 0.6, "x": 20 } ] }
      },
      "events": [
        { "time": 0, "name": "blank" },
        { "time": 0, "name": "step", "int": 1 },
        { "time": 0.3, "name": "step", "int": 2 },
        { "time": 0.3, "name": "sfx" },
        { "time": 0.3, "name": "blank", "string": "third" }
      ]
    }
  }
}
)";
}  // namespace SpineGpuEventFixture
