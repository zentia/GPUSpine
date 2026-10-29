// =====================================================================
// Spine 3.8 json 骨架解析器（测试专用）
// ---------------------------------------------------------------------
// 核心只支持二进制 .skel，json 解析仅用于测试夹具（手写 json 比手写二进制好读）。
// 实现在 SpineGpuImporterJson.inl，由测试侧 #include，与核心共享匿名命名空间
// 里的帮助函数。
// =====================================================================
#pragma once

#include "SpineGpuFormat.h"

SpineImportResult ParseSpineSkeletonJson(const char* json, size_t len, const SpineAtlas* atlas,
                                          SpineSourceSkeleton& dst);
