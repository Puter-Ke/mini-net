#!/usr/bin/env python3
"""同机快速压测（M6 会换成自写 C++ 客户端 + 双机测试，这份只作为即时基线）

用法：
    python3 scripts/bench_quick.py --conns 100 --requests 200 --size 64
输出：QPS、成功/失败数、延迟 P50/P90/P99/P999（微秒）、耗时
"""
import argparse
import asyncio
import statistics
import time


async def worker(host, port, requests, size, latency, errors, barrier):
    try:
        reader, writer = await asyncio.open_connection(host, port)
    except Exception:
        errors.append(1)
        return
    payload = b'x' * size
    await barrier.wait()          # 所有连接就绪后再一起开打，避免测量偏差
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
    barrier = asyncio.Barrier(args.conns)

    t0 = time.perf_counter()
    await asyncio.gather(*[
        worker(args.host, args.port, args.requests, args.size, latency, errors, barrier)
        for _ in range(args.conns)
    ])
    elapsed = time.perf_counter() - t0

    total = args.conns * args.requests
    done = len(latency)

    def pct(p):
        if not latency:
            return float('nan')
        s = sorted(latency)
        return s[min(len(s) - 1, int(len(s) * p / 100))]

    print(f'--- 压测结果 {args.label} ---')
    print(f'连接数        : {args.conns}')
    print(f'每连接请求数  : {args.requests}（消息 {args.size} 字节）')
    print(f'总请求        : {total}，完成 {done}，失败 {len(errors)}')
    print(f'耗时          : {elapsed:.3f} s')
    print(f'QPS           : {done / elapsed:,.0f}')
    print(f'延迟 P50/P90/P99/P999 (us): '
          f'{pct(50):.0f} / {pct(90):.0f} / {pct(99):.0f} / {pct(99.9):.0f}')
    print(f'平均延迟      : {statistics.mean(latency):.1f} us' if latency else '平均延迟      : -')
    print('注意：服务端和客户端同机（4 核），数字偏保守，仅作基线')


if __name__ == '__main__':
    asyncio.run(main())
