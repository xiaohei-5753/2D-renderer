// gfx/renderer.h — 渲染器库的公开入口（libpb_render.a）
//
// ## 这个头文件的存在意义：把"依赖方向"变成可检查的规则
//
//   core/  ->  gfx/  ->  scenes/  (内容)
//                   \->  ui/     (控件)  ->  app/ (画板前端)
//
// **gfx/ 与 core/ 不得包含 app/ 或 ui/ 的任何头文件。** 这条规则以前靠人记，
// 现在靠库边界：`libpb_render.a` 只由 core/+gfx/ 编出来，任何反向依赖都会在
// 链接期缺符号。`tools/` 里的工具（离线渲染、基准、测试）与 `app/` 一样，
// 只是链接这个库的**消费者**，不是它的一部分。
//
// ## 最小用法
//
//   Document doc(320, 180);
//   doc.addLayer("bg");
//   // ... 往 doc.layer(0) 里画像素 ...
//
//   gfx::LightSweep sweep;              // 需要先有一个当前 GL 上下文
//   sweep.init(doc.width(), doc.height());
//   sweep.setLayerCount(doc.layerCount());
//   for (int i = 0; i < doc.layerCount(); i++)
//       sweep.uploadLayer(i, doc.layer(i).albedo, doc.layer(i).material.data(), all);
//   for (int i = 0; i < doc.layerCount(); i++) sweep.renderLayer(i);
//   sweep.composite();
//   GLuint tex = sweep.resultTexture(); // 交给任何显示/导出路径
//
// 画质档位见 LightSweep::FieldQuality：Exact 是原始路径（默认），
// Coarse2/3/4 只降"光场"分辨率，albedo/自发光/合成保持全分辨率。
#pragma once

// --- 数据模型（core）---
#include "core/image.h"
#include "core/layer.h"
#include "core/material.h"
#include "core/types.h"

// --- 渲染器（gfx）---
#include "gfx/denoise.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/layer_composite.h"
#include "gfx/light_sweep.h"
#include "gfx/material_pack.h"
#include "gfx/shader.h"
#include "gfx/viewport.h"
