# 进度与下一步

更新时间：2026-09-14 12:05

## 项目地址
https://github.com/Puter-Ke/mini-net（public，CI 徽章已挂上）

## 开发环境（重要）
本机（Windows）**没有任何 Linux 工具链**：无 WSL 发行版、无 Docker、无 g++/cmake；
Windows 可选组件 VirtualMachinePlatform 因 WU 的 FoD 包下载反复失败而装不上。
→ 所有编译/测试/压测都在 **GitHub Actions** 上跑：

```bash
# 一键验证（构建 + 单测 + 冒烟 + 压测 + 空闲超时演示）
gh workflow run run.yml -f script="bash scripts/remote_m1_bench.sh"
gh run list --workflow=run.yml -L 1
gh run view <ID> --log | grep -A40 "执行脚本"
```
Codespace 已创建但镜像缺 sshd（`gh cs ssh` 不可用），已停止以节省免费额度。

## 已完成
- [x] **M1** 单线程 Reactor + epoll(ET) + 非阻塞 fd + EINTR/SIGPIPE 处理
      → 1000 并发不崩、fd 零泄漏、QPS 29,323（100 连接 / 64B）
- [x] **M2** 周期定时器 `EventLoop::runEvery` + 日志系统 + 空闲连接清理
      → 集成测试 4/4 通过；端到端日志证实僵尸连接被踢掉
- [x] CI（Release 构建 + 单测 + ASAN/UBSan + 1000 并发冒烟）
- [x] Remote Run 工作流（按需 Linux 执行器）
- [x] 文档：新手手册、M1 模块讲解（面试 12 问）、8 周计划、压测记录表、踩坑记录

## 下一步
| 阶段 | 内容 | 状态 |
|---|---|---|
| M3 | one loop per thread + 线程池 + eventfd 唤醒（目标：QPS 翻倍） | 待做 |
| M4 | 环形缓冲 + readv + 内存池（目标：火焰图里 malloc/free < 5%） | 待做 |
| M5 | HTTP/1.1 解析 + 短链业务 + 时间轮做读超时 | 待做 |
| M6 | 自写 C++ 压测客户端 + 双机测试 + 对照实验 | 待做 |
| M7 | pytest 接口测试 + Locust + 混沌用例 + CI 全绿 | 待做 |
| M8 | 架构文档 + 双语 README + 简历三条 + 面试自查 | 待做 |

## 已知待改进（面试时可讲）
- `send` 未处理部分写（需要写缓冲 + EPOLLOUT）→ M4
- `queueInLoop` 还没有 eventfd 唤醒（最坏延迟 100ms）→ M3
- 单线程吃不满多核（QPS 卡在 29k）→ M3
- 日志里的 `__FILE__` 是全路径，可换 `__FILE_NAME__`（GCC 12+）缩短
