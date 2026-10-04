// core/console.h — 控制台输出编码
//
// 源码用 UTF-8 保存，编译时也用 -fexec-charset=UTF-8，所以字符串字面量里是 UTF-8 字节。
// Windows 控制台默认是本地代码页（简体中文环境为 GBK），直接打印会乱码，
// 因此在程序入口先把它切到 UTF-8。
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pb {

inline void setupConsoleUtf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

}  // namespace pb
