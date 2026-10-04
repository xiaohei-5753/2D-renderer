// gfx/gl_common.h — 所有 gfx 翻译单元统一包含 GLEW
//
// GLEW_STATIC 必须在包含 <GL/glew.h> **之前** 定义：否则 glew.h 会把函数声明成
// dllimport，生成 __imp___glew* 形式的引用，静态链接时全部报 undefined reference。
// 放在这个头文件里可以保证每个翻译单元看到的声明都一致，而不依赖构建脚本是否记得
// 传 -DGLEW_STATIC。
#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif

#include <GL/glew.h>
