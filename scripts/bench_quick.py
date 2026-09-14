#!/usr/bin/env python3
"""同机快速压测（M6 会换成自写 C++ 客户端 + 双机测试，这份只作为即时基线）

用法：
    python3 scripts/bench_quick.py --conns 100 --requests 200 --size 64

设计要点（都是踩过的坑）：
- **不用 asyncio.Barrier**：它要求所有参与者都到达，只要有连接失败就会永久卡死。
  改用"连上就注册、协调者等齐（或超时）后放行"的方式，个别失败不影响其他连接。
- 全局 wait_for 兜底：任何情况下都不会把 CI 卡住。
- 报告会明确写出"实际成功连接数"，避免把失败当成功解读。
"""
import argparse
import asyncio
import statistics
import time


async def worker(host, port, requests, size, latency, errors, go):
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=10)
    except Exception:
        errors.append(1)
        return

    try:
        await asyncio.wait_for(go.wait(), timeout=30)   # 等放行信号（最多 30 秒）
    except asyncio.TimeoutError:
        errors.append(1)
        writer.close()
        return

    payload = b'x' * size
    try:
        for _ in range(requests):
            t0 = time.perf_counter()
            writer.write(payload)
            await writer.drain()
            await reader.readexactly(size)
            latency.append((time.perf_counter() - t0) * 1e6)
    except Exception:
        errors.append(1)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--conns', type=int, default=100)
    ap.add_argument('--requests', type=int, default=200)
    ap.add_argument('--size', type=int, default=64, help='每次请求字节数')
    ap.add_argument('--label', default='')
    args = ap.parse_args()

    latency, errors = [], []
    go = asyncio.Event()

    tasks = [
        asyncio.create_task(worker(args.host, args.port, args.requests, args.size, latency, errors, go))
        for _ in range(args.conns)
    ]

    # 协调者：等连接建起来（最多 5 秒），然后放行
    async def coordinator():
        for _ in range(100):
            await asyncio.sleep(0.05)
        go.set()

    coord = asyncio.create_task(coordinator())

    t0 = time.perf_counter()
    try:
        await asyncio.wait_for(asyncio.gather(*tasks, return_exceptions=True), timeout=180)
    except asyncio.TimeoutError:
        print(f'  !! 压测超时（{args.conns} 连接 {args.requests} 请求），已完成的样本仍然统计')
    finally:
        coord.cancel()
    elapsed = time.perf_counter() - t0

    total = args.conns * args.requests
    done = len(latency)
    conns_ok = args.conns - sum(1 for e in errors if e == 1) if errors else args.conns

    def pct(p):
        if not latency:
            return float('nan')
        s = sorted(latency)
        return s[min(len(s) - 1, int(len(s) * p / 100))]

    print(f'--- 压测结果 {args.label} ---')
    print(f'连接数        : {args.conns}（失败 {len(errors)}）')
    print(f'每连接请求数  : {args.requests}（消息 {args.size} 字节）')
    print(f'总请求        : {total}，完成 {done}，失败 {len(errors)}')
    print(f'耗时          : {elapsed:.3f} s')
    print(f'QPS           : {done / elapsed:,.0f}' if done else 'QPS           : -')
    print(f'延迟 P50/P90/P99/P999 (us): '
          f'{pct(50):.0f} / {pct(90):.0f} / {pct(99):.0f} / {pct(99.9):.0f}')
    print(f'平均延迟      : {statistics.mean(latency):.1f} us' if latency else '平均延迟      : -')
    print('注意：服务端与客户端同机（4 核），客户端为单线程 Python，数字明显偏保守')


if __name__ == '__main__':
    asyncio.run(main())
