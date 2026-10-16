// GBK(936) <-> UTF-8 编码转换 —— 全项目唯一实现(P5 收敛, 此前有 4 份拷贝:
// UiText/TabGam/ItemData/Summon*)。游戏内字符串固定 GBK, 界面为 UTF-8。
// 语义: 输入空串返回空串; 编码失败返回空串(调用方按需兜底), 不抛异常。
#pragma once

#include <string>

namespace poq {

std::string Utf8ToGbk(const std::string& u8);
std::string GbkToUtf8(const std::string& gbk);

}  // namespace poq
