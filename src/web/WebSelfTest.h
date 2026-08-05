/**
 * 文件作用：声明 ChatGPT Web 监听阶段自检命令
 * 职责范围：
 * 1. 用内存数据验证状态机、去重和聚合规则
 * 2. 输出 P1-P6 的可读自检结果
 *
 * 不负责：
 * - 连接真实 Chrome
 * - 替代真实浏览器验收
 *
 * 维护说明：
 * - 自检覆盖核心不变量，真实浏览器验收由扩展和 Native Host 链路完成
 */
#pragma once

class WebSelfTest {
public:
    int Run();
};
