> 本工具仅供网络管理员、合规安全审计人员在拥有明确授权的受控网络、虚拟实验室或自建靶场环境内进行资产盘点与连通性验证。使用者应对执行本工具所引发的一切直接或间接法律纠纷、技术故障及网络影响承担全部独立责任。如需在公网或非属主网段运行，必须确保已事先取得相关权威主体的书面授权。

`lslanpage` 专为大规模内网资产梳理设计，核心目标是在**常驻低损耗**、**零磁盘 I/O** 与 **内存绝对受限** 的前提下，完成对任意规模子网（如 `/24`、`/16` 甚至 `/8`）的 Web 端口（80/443）探测。

### 内存炸不了
- 全局仅维持一个 `uint64_t g_current_task_idx` 计数器。
- 线程获取当前索引后，通过位运算即时计算目标。
- 无论是 `/30` 还是 `/8` 网段，程序堆内存占用恒定维持在毫秒级的局部变量与线程栈（通常小于 15MB）。

### 跨平台的
- **Linux (Debian/Ubuntu)**：使用 `pthread` 库，主线程通过 `pthread_create` 派生子线程，通过 `pthread_join` 等待本轮汇聚。
- **Windows (10/11)**：基于 `<process.h>` 中的 `_beginthreadex` 派生底层线程句柄，通过 `WaitForSingleObject` 进行多句柄同步等待，避免使用跨平台模拟层带来的运行时性能损耗。
- **互斥控制**：Linux 下使用 `pthread_mutex_t`，Windows 下使用低开销的临界区 `CRITICAL_SECTION`，分别保护任务索引消费与标准控制台输出。

### 策略层
- **底层引擎**：每个工作线程独立维系一个 `CURL*` 会话句柄，重复复用 TCP 连接池。
- **证书宽容策略**：默认关闭对 TLS 证书链与主机名的校验（`CURLOPT_SSL_VERIFYPEER = 0`，`CURLOPT_SSL_VERIFYHOST = 0`），确保内网路由器、交换机及自签证书管理端能无阻碍抓取明文页面。
- **严格超时熔断**：单次握手与传输由 `-wait` 强制限制（默认建议 2200ms），线程绝不无限期阻塞于失联主机。

### 输出不炸
- **内存保护截断**：通过 `write_callback` 数据流入回调函数，对响应体进行容量裁剪。当接收到的数据量超出 `-see` 指定阈值时，多余数据仅计算返回大小供底层状态机流转，不再追加写入内存缓冲区，防止恶意大文件（如固件镜像）撑爆进程。
- **UTF-8 跨平台兼容**：
  - Windows 平台通过 `SetConsoleOutputCP(CP_UTF8)` 强制修改当前终端代码页。
  - POSIX 平台通过 `setlocale(LC_ALL, "")` 采用环境原生宽字符流，消除中文乱码。
- **纯文本输出**：剥离全部 ANSI Escape 颜色转义字符，确保输出至基础终端或重定向时排版稳定整洁。

## 引入头文件与 API/函数清单

### C 标准库与核心系统头文件

| 头文件 | 核心函数 / 类型 / 宏 | 功能用途 |
| :--- | :--- | :--- |
| `<stdio.h>` | `printf()`, `snprintf()`, `sscanf()`, `fflush()` | 控制台无缓冲输出、IP 字符串格式化、CIDR 解析 |
| `<stdlib.h>` | `malloc()`, `free()`, `atoi()`, `atol()` | 动态分配响应截断缓冲区、解析 CLI 数值参数 |
| `<string.h>` | `strcmp()`, `memcpy()`, `memset()` | CLI 参数名比对、网络流数据内存拷贝 |
| `<stdint.h>` | `uint32_t`, `uint64_t` | 跨平台定宽整数，支持大子网寻址计算 |
| `<signal.h>` | `signal()`, `sig_atomic_t`, `SIGINT`, `SIGTERM` | 注册终端中断事件处理函数，控制优雅停机 |

### 平台特定接口（条件编译分支）

#### Linux / POSIX 平台
| 头文件 | 核心接口 / 函数 | 功能用途 |
| :--- | :--- | :--- |
| `<unistd.h>` | `sleep()` | 执行秒级休眠轮询 |
| `<pthread.h>` | `pthread_create()`, `pthread_join()`<br>`pthread_mutex_init()`, `pthread_mutex_lock()`<br>`pthread_mutex_unlock()`, `pthread_mutex_destroy()` | 多线程创建与汇合回收；保证任务索引与终端打印的原子性 |
| `<locale.h>` | `setlocale(LC_ALL, "")` | 初始化操作系统本地化字符集（默认 UTF-8） |
| `<arpa/inet.h>` / `<netinet/in.h>` | `uint32_t` 字节序转换及协议宏 | 辅助网络参数结构定义 |

#### Windows 平台
| 头文件 | 核心接口 / 函数 | 功能用途 |
| :--- | :--- | :--- |
| `<windows.h>` | `SetConsoleOutputCP()`, `SetConsoleCP()`<br>`Sleep()`, `WaitForSingleObject()`, `CloseHandle()` | 强制设置终端为 UTF-8 代码页；毫秒级休眠；线程句柄状态监听与回收 |
| `<process.h>` | `_beginthreadex()` | 创建符合 C 运行时库安全标准的 Win32 线程 |
| `<winsock2.h>` / `<ws2tcpip.h>` | 基础 Windows 网络架构支持 | 为底层 libcurl 与套接字栈提供依赖声明 |
| 临界区宏映射 | `InitializeCriticalSection()`, `EnterCriticalSection()`<br>`LeaveCriticalSection()`, `DeleteCriticalSection()` | Win32 极轻量级进程内线程锁 |

### 第三方网络库：libcurl

| 函数 / 选项 | 类型 | 功能用途 |
| :--- | :--- | :--- |
| `curl_global_init()` | 全局初始化 | 初始化网络套接字环境及 TLS 后端引擎（进程生命周期内一次） |
| `curl_global_cleanup()` | 全局清理 | 释放底层网络栈全局共享资源 |
| `curl_easy_init()` | 会话创建 | 每个线程分配独立的请求上下文 |
| `curl_easy_cleanup()` | 会话销毁 | 回收当前线程分配的 curl 资源 |
| `curl_easy_perform()` | 阻塞请求 | 执行完整的 HTTP 握手、数据接收及状态解析流程 |
| `curl_easy_getinfo()` | 信息获取 | 提取响应状态码（`CURLINFO_RESPONSE_CODE`） |
| **`curl_easy_setopt()` 核心配置** | | |
| `CURLOPT_URL` | 参数配置 | 设置目标 `http://...` 或 `https://...` 完整路径 |
| `CURLOPT_CONNECTTIMEOUT_MS` | 参数配置 | 设置底层 TCP 连接超时阈值 |
| `CURLOPT_TIMEOUT_MS` | 参数配置 | 设置整个请求/传输流程的总超时限制 |
| `CURLOPT_SSL_VERIFYPEER` | 参数配置 | 置 `0L` 禁用对证书链合法性的强校验 |
| `CURLOPT_SSL_VERIFYHOST` | 参数配置 | 置 `0L` 禁用对证书 CN/SAN 与主机的匹配校验 |
| `CURLOPT_FOLLOWLOCATION` | 参数配置 | 启用自动跟随 `301/302` 重定向 |
| `CURLOPT_MAXREDIRS` | 参数配置 | 限制重定向层级（默认 3 层），防止跳转环路死锁 |
| `CURLOPT_NOSIGNAL` | 参数配置 | 多线程环境下必须置 `1L`，防止 libcurl DNS 触发 POSIX 信号干扰主线程 |
| `CURLOPT_WRITEFUNCTION` | 参数配置 | 绑定数据流入截断处理函数 |
| `CURLOPT_WRITEDATA` | 参数配置 | 传递线程私有的局部响应缓存区指针 |

---

### Linux 静态编译
```bash
sudo apt update && sudo apt install -y build-essential pkg-config libcurl4-openssl-dev libssl-dev zlib1g-dev libnghttp2-dev libngtcp2-dev libnghttp3-dev libssh2-1-dev libpsl-dev libbrotli-dev libzstd-dev libidn2-dev libunistring-dev libldap-dev libkrb5-dev libgpg-error-dev && gcc -O2 -Wall -pthread lslanpage.c -o lslanpage -Wl,-Bstatic -lcurl -Wl,-Bdynamic $(pkg-config --static --libs libcurl) -ldl
```

### Windows 静态编译
```bash
# MSYS2 控制台下安装依赖
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-curl-winssl mingw-w64-x86_64-openssl
gcc -O2 -Wall lslanpage.c -o lslanpage.exe -static -lcurl -lssl -lcrypto -lws2_32 -lcrypt32 -lwldap32 -lpthread
```

### 参数表

| 参数名称 | 类型 | 示例值 | 说明 |
| :--- | :--- | :--- | :--- |
| `-ip` | 字符串 | `192.168.10.0/24` | 目标网段（支持 `/8` 到 `/32` 任意标准 CIDR 格式） |
| `-th` | 整型 | `12` | 并发探测线程数上限 |
| `-wait` | 长整型 | `2200` | 单次 HTTP/HTTPS 请求超时上限（单位：毫秒） |
| `-sleep` | 整型 | `36000` | 完整遍历一轮网段后的挂起等待间隔（单位：秒） |
| `-see` | 整型 | `2048` | 单个成功响应页面打印到终端的最大截断字节数 |

### 跑起来的示例格式

```bash
# Linux
./lslanpage -ip 192.168.1.0/24 -th 12 -wait 2200 -sleep 36000 -see 2048

# Windows
lslanpage.exe -ip 192.168.1.0/24 -th 12 -wait 2200 -sleep 36000 -see 2048
```

```bash
./lslanpage -ip 172.16.0.0/16 -th 30 -wait 2000 -sleep 43200 -see 4096
```

```bash
./lslanpage -ip 10.0.0.0/8 -th 50 -wait 1800 -sleep 86400 -see 1024
```
