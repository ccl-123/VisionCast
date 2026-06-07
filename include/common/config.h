/**
 * @file config.h
 * @brief VisionCast Configuration Management
 * 
 * 本文件声明了 VisionCast 系统配置的管理函数。系统配置涵盖视频捕获、音频捕获、
 * 编码参数（MPP/编码器）、传输流协议设置以及调试选项等。
 * 它提供了默认配置生成、从自定义 JSON 文本文件中解析配置，以及配置汇总摘要输出的接口。
 */

#pragma once

#include <string>

#include "common/types.h"

namespace visioncast {

/**
 * @brief 获取系统的默认配置（包含视频、音频、编码、传输及调试的默认参数）。
 * @return 填充了默认参数的 VisionCastConfig 结构体。
 */
VisionCastConfig default_config();

/**
 * @brief 从指定路径加载并解析 JSON 配置文件。
 * 
 * 解析器为轻量级的手动解析实现，会读取该配置文件并根据其内容覆盖默认参数。
 * 
 * @param path 配置文件所在的本地磁盘路径。
 * @param config 输出参数，用于接收解析成功的配置信息。
 * @param error 输出参数，在解析或读取失败时，填充详细的错误原因。
 * @return 解析并加载成功返回 true，否则返回 false 并写入错误信息到 error 中。
 */
bool load_config_file(const std::string& path, VisionCastConfig& config, std::string& error);

/**
 * @brief 序列化当前配置对象，输出包含各项核心参数的易读文本摘要。
 * 
 * 适用于程序启动时或配置变更时将当前配置打印到日志中，便于诊断。
 * 
 * @param config 需要被汇总的配置结构体。
 * @return 格式化后的多行配置摘要字符串。
 */
std::string summarize_config(const VisionCastConfig& config);

/**
 * @brief 辅助字符串替换函数。
 * 
 * 将字符串中所有指定的子串替换为目标子串。
 * 
 * @param str 需要执行替换操作的源字符串引用。
 * @param from 被替换的子序列。
 * @param to 用于替换的新子序列。
 */
void replace_all(std::string& str, const std::string& from, const std::string& to);

}  // namespace visioncast
