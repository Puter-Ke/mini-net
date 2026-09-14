// TODO(M6): 压测客户端。要求：
//  1. 可配置：并发连接数 / 每连接请求数 / 消息大小 / 是否 pipelining
//  2. 输出：总吞吐(QPS)、成功/失败数、延迟 P50/P90/P99/P999
//  3. 用非阻塞 connect + epoll 自己实现，别用 ab（ab 单连接测不出 C10K）
//  4. 记录环境：CPU 型号/核数、发送与压测是否同机（同机会失真，要说明）
#include <cstdio>

int main() {
    std::printf("未实现：先跑 M1-M4，再来写压测客户端\n");
    return 0;
}
