#!/usr/bin/env python3
"""同机快速压测（M6 会用自写 C++ 客户端 + 双机测试替换这份基线）

用法：python3 scripts/bench_quick.py --conns 500 --requests 100 --size 256

设计要点（都是踩过的坑）：
- 不用 asyncio.Barrier：只要有一个连接失败就会全员卡死；改为"连上就计数、等齐或超时后放行"。
- **只统计真正的施压时间段**：从第一条请求发出到最后一条请求结束，
  连接建立和放行等待的时间都不算进 QPS，否则数字会被严重低估。
- 全局 wait_for 兜底，任何情况都不会挂住 CI。
"""
import argparse
import asyncio
import statistics
import time


async def worker(host, port, requests, size, conns_ready, go, samples, errors):
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=10)
    except Exception:
        errors.append('connect')
        return
    conns_ready[0] += 1

    try:
        await asyncio.wait_for(go.wait(), timeout=30)
    except asyncio.TimeoutError:
        errors.append('barrier')
        writer.close()
        return

    payload = b'x' * size
    local = []
    try:
        for _ in range(requests):
            t0 = time.perf_counter()
            writer.write(payload)
            await writer.drain()
            await reader.readexactly(size)
            local.append((time.perf_counter() - t0) * 1e6)
    except Exception:
        errors.append('io')
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass

    if local:
        samples.append((min(local), local, time.perf_counter()))


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--conns', type=int, default=100)
    ap.add_argument('--requests', type=int, default=200)
    ap.add_argument('--size', type=int, default=64)
    ap.add_argument('--label', default='')
    ap.add_argument('--wait-conns-ms', type=int, default=2000, help='等待连接建立的毫秒数')
    args = ap.parse_args()

    conns_ready = [0]
    errors = []
    samples = []
    go = asyncio.Event()

    tasks = [
        asyncio.create_task(worker(args.host, args.port, args.requests, args.size,
                                   conns_ready, go, samples, errors))
        for _ in range(args.conns)
    ]

    # 协调者：连接数达到 98% 或超时（默认 2 秒）就放行
    t_release = None
    target = max(1, int(args.conns * 0.98))
    deadline = time.perf_counter() + args.wait_conns_ms / 1000.0
    while time.perf_counter() < deadline and conns_ready[0] < target:
        await asyncio.sleep(0.02)
    t_release = time.perf_counter()
    go.set()

    t_wall0 = time.perf_counter()
    try:
        await asyncio.wait_for(asyncio.gather(*tasks, return_exceptions=True), timeout=180)
    except asyncio.TimeoutError:
        print(f'  !! 压测超时（{args.conns} 连接 × {args.requests} 请求），已完成的样本仍然统计')
    t_wall1 = time.perf_counter()

    latency = [v for (_, arr, _e) in samples for v in arr]
    done = len(latency)
    span = (t_wall1 - t_release) if samples else (t_wall1 - t_wall0)

    def pct(p):
        if not latency:
            return float('nan')
        s = sorted(latency)
        return s[min(len(s) - 1, int(len(s) * p / 100))]

    print(f'--- 压测结果 {args.label} ---')
    print(f'连接数        : {args.conns}（成功建立 {conns_ready[0]}，错误 {len(errors)}）')
    print(f'每连接请求数  : {args.requests}（消息 {args.size} 字节）')
    print(f'总请求        : {args.conns * args.requests}，完成 {done}')
    print(f'施压耗时      : {span:.3f} s')
    print(f'QPS           : {done / span:,.0f}' if done and span > 0 else 'QPS           : -')
    print(f'延迟 P50/P90/P99/P999 (us): '
          f'{pct(50):.0f} / {pct(90):.0f} / {pct(99):.0f} / {pct(99.9):.0f}')
    print(f'平均延迟      : {statistics.mean(latency):.1f} us' if latency else '平均延迟      : -')
    print('注意：服务端与客户端同机（4 核），客户端是单线程 Python，数字明显偏保守')


if __name__ == '__main__':
    asyncio.run(main())
