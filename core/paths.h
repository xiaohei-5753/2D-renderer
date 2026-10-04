// core/paths.h — 资源路径解析
//
// 为什么需要这一层：程序里加载资源用的是相对路径（例如 "unifont-17.0.04.otf"），
// 而相对路径是相对**当前工作目录**的，不是相对可执行文件的。
// 从项目根目录运行能读到，双击 build\paintboard.exe 时工作目录变成 build\，
// 就读不到了——表现为字体加载失败、界面中文全变成 '?'。
//
// 所以资源查找要按「可执行文件目录 -> 其上一级 -> 当前工作目录」的顺序去找。
#pragma once

#include <string>

namespace pb {

// 可执行文件所在目录，末尾带路径分隔符。失败时返回空串。
std::string executableDir();

// 查找资源文件。依次尝试：
//   1. 当前工作目录（便于用同名文件覆盖）
//   2. 可执行文件所在目录
//   3. 可执行文件目录的上一级（build/ 的上一级就是项目根，最常见的情况）
// 返回第一个存在的路径；都找不到返回空串。
std::string findAsset(const std::string& relativePath);

// Windows 文件名要用宽字符，这两个是 UTF-8 与 UTF-16 之间的转换
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);

}  // namespace pb
