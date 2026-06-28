#pragma once

#include <string>

namespace hps {

/**
 * URL 百分比解码
 *
 * 将 URL 编码字符串转为原始字符串：
 * - %XX → 对应字节（十六进制，大小写不敏感）
 * - + → 空格
 * - 其他字符保持不变
 *
 * @param src URL 编码字符串
 * @param dst [out] 解码后字符串
 * @return true 解码成功；false 输入格式错误（如 %XX 中 XX 不是合法十六进制）
 */
bool url_decode(const std::string& src, std::string& dst);

} // namespace hps
