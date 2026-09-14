# mini-net — 从零手写的 C++ 高并发网络服务

[![CI](https://github.com/Puter-Ke/mini-net/actions/workflows/ci.yml/badge.svg)](https://github.com/Puter-Ke/mini-net/actions/workflows/ci.yml)

> 当前进度：**M1 完成并通过 1000 并发验证**（QPS 29k / fd 零泄漏 / ASAN 干净）

> 目标：不依赖任何第三方网络框架，用 C++17 + epoll 实现一个支持 10k 并发连接的服务端，
> 并配齐单测、CI、压测报告。这是给大厂 C++ 岗（华为 2012/ICT/海思 等）面试准备的主项目。

## 为什么做这个
- 华为 JD：「掌握常用的软件架构模式、基本的编程编译工具」——只有亲手写过 Reactor / 内存池 / 线程池，
  才能在面试里回答"为什么这么设计、代价是什么"。
- 面试必问：epoll 边缘触发 vs 水平触发、惊群、粘包拆包、内存池为什么不直接用 malloc、
  锁的粒度、time_wait、TCP_NODELAY …… 每一个你都应该能给出自己踩过的坑。

## 里程碑（详细到周见 docs/WEEKLY_PLAN.md）
- [ ] M1 单线程 Reactor + epoll 回显服务器（1k 并发无泄漏）
- [ ] M2 定时器/时间轮 + 日志（空闲连接自动踢掉）
- [ ] M3 one-loop-per-thread 多线程 + 线程池（QPS 提升 ≥2x）
- [ ] M4 环形缓冲 Buffer + 内存池（perf 火焰图 malloc/free <5%）
- [ ] M5 HTTP/1.1 解析 + 短链业务接口
- [ ] M6 压测与优化（产出 QPS / P99 对比报告 + 火焰图）
- [ ] M7 自动化测试套件（单测覆盖率 ≥70% + pytest 接口/性能/混沌用例 + CI 全绿）
- [ ] M8 架构文档 + 中英双语 README + 简历三条量化描述

## 环境（WSL2 Ubuntu，一条命令装完）
```bash
wsl --install -d Ubuntu          # Windows 侧执行一次，需要重启
# 进入 Ubuntu 后：
bash scripts/env-setup.sh
```

## 构建 / 运行 / 测试
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mini_net_server 8080            # 启动服务
ctest --test-dir build --output-on-failure
cmake -B build-asan -DENABLE_ASAN=ON && cmake --build build-asan -j   # 内存检查
```

## 目录
```
include/mini_net/   公共头文件（接口在这里定义，实现由你写）
src/                实现
tests/              GoogleTest 单元测试
bench/              压测客户端与压测脚本
docs/               架构文档、周计划、面试自查清单
scripts/            环境安装、压测、CI 辅助脚本
```
