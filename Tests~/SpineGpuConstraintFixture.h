#pragma once

// =====================================================================
// 约束参数时间轴夹具（3.8 json）：IK / transform 约束时间轴 + path spacing 时间轴。
// SpineGpuImportTest（手算断言，进 ctest）与 SpineGpuRefParityTest（与 spine-cpp 随机脚本逐帧对拍）共用。
//
// 为什么必须手写：OSGame 782 个资产里**用到这三类时间轴的资产数是 0**（扫描结论）。
// 也就是说这三条 apply 分支在真实资产上永远走不到 —— 没有夹具就等于没有验证。
// 而它们又是"多轨高级混合"里最典型的参数轨：轨道 A 淡出时它的 IK mix / transform mix
// 要从动画值渐变回 setup，解算器读的是**运行时**值而不是 setup 常量。
//
// 夹具刻意满足这几条（缺一条就验不出问题）：
//   * 每类时间轴 **≥ 3 帧**且第 0 帧时间不为 0（tf2 首帧 0.2）⇒ 插值段 / `time < 首帧`
//     两个分支都走得到；
//   * 帧值**不等于 setup**（setup 的 IK mix = 0.35、transform 四 mix = 0.15/0.25/0.35/0.45、
//     path spacing = 5；动画里全给别的值）⇒ "解算器读成 setup 常量"立刻分叉；
//   * 曲线段 **bezier + stepped 各一条**；
//     ⚠️ 曲线只能写在**非末帧**上（`CurveTimeline.cpp:78` 断言 `frameIndex < _segmentCount`，
//        段数 = 帧数 - 1）⇒ 带曲线的那类时间轴必须 ≥ 3 帧，否则参考实现直接 assert 退出。
//     ⚠️ 曲线作用的段号 = **写它的那一帧的下标**（段 i = 帧 i → 帧 i+1），不是"这一帧之前"；
//        path spacing 的曲线写在**首帧**上，就是为了让 [0.6, 1.0] 这段保持线性可手算。
//   * 动画里的 path 时间轴段键名是 **`"paths"`**（约束**定义**那一段才是 `"path"`）。写错成
//     `"path"` 时**两边都静默忽略**（SkeletonJson.cpp:770 只认 `root.paths`）⇒ 时间轴凭空
//     消失、没有任何报错 —— 第一次写夹具就踩了，表现是 spacing 恒等于 setup。
//   * IK 的 `bendPositive` / `compress` / `stretch` 在帧间**真的翻转**（±1、0/1）⇒
//     参考实现"末帧弯曲方向"的笔误（IkConstraintTimeline.cpp:131/138 读的是
//     `_frames.size() + PREV_BEND_DIRECTION`）只有翻了才看得出来；
//   * 两个动画（con_a / con_b）都写这三类 ⇒ 多轨交叉淡入淡出时 MixDirection_Out 分支会走到。
//
// 骨骼：root(0) → ik_a(1) → ik_b(2)；root → ik_target(3)（IK 目标）；
//       root → tf_target(4)（transform 目标）；root → tf_a(5) → tf_b(6)；root → path_bone(7)。
// 槽位：sp（path 附件，给 p1 用）、sa（region，挂 ik_b）、sb（region，挂 tf_b）。
// =====================================================================

namespace SpineGpuConstraintFixture
{
    inline const char* kAtlas = "constraints.png\n"
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
                                "  size: 24, 16\n";

    inline const char* kSkeleton = R"({
  "skeleton": { "spine": "3.8.99" },
  "bones": [
    { "name": "root" },
    { "name": "ik_a", "parent": "root", "x": 20, "y": 0, "length": 40 },
    { "name": "ik_b", "parent": "ik_a", "x": 40, "y": 0, "length": 30 },
    { "name": "ik_target", "parent": "root", "x": 50, "y": -10, "length": 10 },
    { "name": "tf_target", "parent": "root", "x": -30, "y": 15, "rotation": 25,
      "scaleX": 1.3, "scaleY": 0.7, "shearX": 8, "shearY": -6, "length": 20 },
    { "name": "tf_a", "parent": "root", "x": -10, "y": -20, "rotation": -15, "length": 25 },
    { "name": "tf_b", "parent": "tf_a", "x": 25, "rotation": 40, "length": 20 },
    { "name": "path_bone", "parent": "root", "x": 10, "y": -40, "length": 15 }
  ],
  "slots": [
    { "name": "sp", "bone": "root", "attachment": "track" },
    { "name": "sa", "bone": "ik_b", "attachment": "chip_a" },
    { "name": "sb", "bone": "tf_b", "attachment": "chip_b" }
  ],
  "ik": [
    { "name": "ik1", "order": 0, "bones": [ "ik_a", "ik_b" ], "target": "ik_target",
      "mix": 0.35, "bendPositive": false, "compress": true, "stretch": false, "uniform": false }
  ],
  "transform": [
    { "name": "tf1", "order": 1, "bones": [ "tf_a", "tf_b" ], "target": "tf_target",
      "rotation": 30, "x": 12, "y": -7, "scaleX": 1.4, "scaleY": 0.6, "shearY": 18,
      "rotateMix": 0.15, "translateMix": 0.25, "scaleMix": 0.35, "shearMix": 0.45,
      "local": false, "relative": false },
    { "name": "tf2", "order": 2, "bones": [ "tf_b" ], "target": "tf_target",
      "rotation": -20, "x": 4, "y": 9, "scaleX": 0.8, "scaleY": 1.25, "shearY": -12,
      "rotateMix": 0.55, "translateMix": 0.65, "scaleMix": 0.75, "shearMix": 0.85,
      "local": true, "relative": true }
  ],
  "path": [
    { "name": "p1", "order": 3, "bones": [ "path_bone" ], "target": "sp",
      "positionMode": "percent", "spacingMode": "length", "rotateMode": "tangent",
      "position": 0.2, "spacing": 5, "rotateMix": 1, "translateMix": 1 }
  ],
  "skins": {
    "default": {
      "sp": { "track": { "type": "path", "closed": false, "constantSpeed": false,
                         "vertexCount": 6, "vertices": [ -20,0, -10,4, 0,0,  40,12, 55,24, 70,36 ],
                         "lengths": [ 35, 70 ] } },
      "sa": { "chip_a": { "path": "r1", "x": 4, "y": 1, "width": 18, "height": 14, "rotation": 5 } },
      "sb": { "chip_b": { "path": "r2", "x": -2, "y": 3, "width": 12, "height": 9 } }
    }
  },
  "animations": {
    "move": {
      "bones": {
        "ik_target": { "translate": [ { "time": 0, "x": 0, "y": 0 },
                                      { "time": 0.5, "x": 25, "y": 18 },
                                      { "time": 1.0, "x": -12, "y": 6 } ] }
      }
    },
    "con_a": {
      "ik": {
        "ik1": [
          { "time": 0.0, "mix": 0.05, "bendPositive": true,  "compress": false, "stretch": false },
          { "time": 0.4, "mix": 0.95, "bendPositive": false, "compress": true,  "stretch": true,
            "curve": [ 0.25, 0.1, 0.75, 0.9 ] },
          { "time": 1.0, "mix": 0.5,  "bendPositive": true,  "compress": false, "stretch": true }
        ]
      },
      "transform": {
        "tf1": [
          { "time": 0.0, "rotateMix": 0.05, "translateMix": 0.15, "scaleMix": 0.25, "shearMix": 0.35 },
          { "time": 0.5, "rotateMix": 0.85, "translateMix": 0.75, "scaleMix": 0.65, "shearMix": 0.55,
            "curve": "stepped" },
          { "time": 1.0, "rotateMix": 0.4,  "translateMix": 0.3,  "scaleMix": 0.2,  "shearMix": 0.1 }
        ],
        "tf2": [
          { "time": 0.2, "rotateMix": 0.1, "translateMix": 0.2, "scaleMix": 0.3, "shearMix": 0.4 },
          { "time": 0.8, "rotateMix": 0.9, "translateMix": 0.8, "scaleMix": 0.7, "shearMix": 0.6,
            "curve": [ 0.1, 0.8, 0.9, 0.2 ] },
          { "time": 1.0, "rotateMix": 0.35, "translateMix": 0.45, "scaleMix": 0.5, "shearMix": 0.15 }
        ]
      },
      "paths": {
        "p1": {
          "spacing": [ { "time": 0.0, "spacing": 0, "curve": [ 0.3, 0.0, 0.7, 1.0 ] },
                       { "time": 0.6, "spacing": 25 },
                       { "time": 1.0, "spacing": 8 } ],
          "position": [ { "time": 0.0, "position": 0.1 },
                        { "time": 1.0, "position": 0.9 } ]
        }
      }
    },
    "con_b": {
      "ik": {
        "ik1": [
          { "time": 0.0, "mix": 1.0,  "bendPositive": false, "compress": true,  "stretch": true },
          { "time": 0.7, "mix": 0.0,  "bendPositive": true,  "compress": false, "stretch": false,
            "curve": "stepped" },
          { "time": 1.0, "mix": 0.75, "bendPositive": false, "compress": true,  "stretch": false }
        ]
      },
      "transform": {
        "tf1": [
          { "time": 0.0, "rotateMix": 1.0, "translateMix": 0.0, "scaleMix": 0.9, "shearMix": 0.05 },
          { "time": 0.9, "rotateMix": 0.2, "translateMix": 1.0, "scaleMix": 0.1, "shearMix": 0.95 }
        ],
        "tf2": [
          { "time": 0.3, "rotateMix": 0.55, "translateMix": 0.45, "scaleMix": 0.65, "shearMix": 0.75 }
        ]
      },
      "paths": {
        "p1": {
          "spacing": [ { "time": 0.0, "spacing": 40 },
                       { "time": 0.5, "spacing": 5 } ]
        }
      }
    }
  }
})";
}   // namespace SpineGpuConstraintFixture
