# 进度与下一步（重启后看这里）

更新时间：2026-09-14

## 当前状态
- [x] 项目骨架（26 个文件）：头文件接口、CMake、测试框架、CI 模板、压测占位
- [x] **M1 实现代码已写完**（689 行）：EventLoop / EpollPoller / Channel / TcpServer / Buffer.readFd
- [x] 新手执行手册 `docs/新手第一步.md`、验收脚本 `scripts/smoke_test.sh`
- [x] M1 讲解文档 `docs/M1模块讲解.md`（面试问答 12 题）
- [ ] **代码尚未编译验证**（本机无 Linux 工具链，等 WSL 装完）
- [x] 已诊断出 WSL 装不上的根因：**Windows 可选组件 VirtualMachinePlatform 未启用**
      （`wsl --status` 报"未启用虚拟化"；CPU 锐龙 7 5825U 支持虚拟化，WSL 应用 2.7.13 已装好）
- [~] 正在用 DISM 启用组件（`dism /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart`
      + `Microsoft-Windows-Subsystem-Linux`），CBS 正在从 Windows Update 下载组件包 → **装完必须重启**
- [ ] 重启后：安装发行版 → 编译 → 跑测试 → 拿到第一组数字

## 重启后的完整命令（按顺序）
```powershell
# 1) Windows 里：优先用你 Downloads 里已有的镜像，省一次下载
wsl --install --from-file "C:\Users\32031\Downloads\noble-wsl-amd64.wsl"
#    如果上面不支持，就退回到：
wsl --install -d Ubuntu
```
```bash
# 2) 进 Ubuntu 后
mkdir -p ~/projects
cp -r /mnt/c/Users/32031/Desktop/mini-net ~/projects/mini-net
cd ~/projects/mini-net
bash scripts/env-setup.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
bash scripts/smoke_test.sh 8080
```

## 如果 WSL2 最终仍起不来（备用方案）
1. 买一台学生云服务器（阿里云/腾讯云 10 元/月，Ubuntu 22.04），我把代码传上去编译运行——
   顺带把 M8 的"线上可访问"也做了；
2. 退回 WSL1（只需 WSL 组件，不需要虚拟化）：`wsl --set-default-version 1`，
   但 epoll 在 WSL1 上是模拟的，行为可能和真 Linux 有差异，仅作临时方案。

## 重启后的第一步（照顺序执行）
```powershell
# Windows 侧
wsl -l -v                      # 确认 Ubuntu 已注册
```
```bash
# 进入 Ubuntu 后
mkdir -p ~/projects
cp -r /mnt/c/Users/32031/Desktop/mini-net ~/projects/mini-net
cd ~/projects/mini-net
bash scripts/env-setup.sh        # 已改成清华源
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
bash scripts/smoke_test.sh 8080
```

## 接下来（M2-M8）
| 阶段 | 内容 | 谁来做 |
|---|---|---|
| M2 | 时间轮接进 EventLoop，空闲连接超时踢掉；日志 | 待定 |
| M3 | one loop per thread + 线程池 + eventfd 唤醒 | 待定 |
| M4 | 环形缓冲 + 内存池 + readv | 待定 |
| M5 | HTTP/1.1 解析 + 短链业务 | 待定 |
| M6 | 压测客户端 + 对照实验 + 火焰图 | 待定 |
| M7 | pytest 接口测试 + Locust + 混沌 + CI | 待定 |
| M8 | 架构文档 + 双语 README + 简历 | 待定 |

## 未编译代码的风险清单（编译时重点看这几处）
- `TcpServer::Conn` 用 `std::shared_ptr` + 前置声明，`~TcpServer` 在 .cc 里定义（顺序要能过编译）
- `Channel` 的构造函数已改成 `Channel(int fd, EventLoop* loop)`，`tests/test_event_loop.cc` 与新签名一致
- `Buffer::readFd/writeFd` 现在是真的能用的实现（M4 再升级）
- `main.cc` 的 `setConnectionCallback` 里 alive 参数打印成了固定 0，编译能过但显示不对（M2 修）
