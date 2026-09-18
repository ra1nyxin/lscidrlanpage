#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <curl/curl.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>
    #define sleep_sec(x) Sleep((DWORD)((x) * 1000))
    typedef HANDLE pthread_t;
    typedef CRITICAL_SECTION pthread_mutex_t;
    #define pthread_mutex_init(m, a) InitializeCriticalSection(m)
    #define pthread_mutex_lock(m) EnterCriticalSection(m)
    #define pthread_mutex_unlock(m) LeaveCriticalSection(m)
    #define pthread_mutex_destroy(m) DeleteCriticalSection(m)
#else
    #include <unistd.h>
    #include <pthread.h>
    #include <locale.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #define sleep_sec(x) sleep(x)
#endif

static uint32_t g_start_ip = 0;
static uint32_t g_end_ip = 0;
static uint64_t g_total_hosts = 0;
static uint64_t g_total_tasks = 0;
static int g_threads = 0;
static long g_timeout_ms = 0;
static int g_sleep_interval = 0;
static size_t g_max_see_bytes = 0;
static uint64_t g_current_task_idx = 0;
static pthread_mutex_t g_task_mutex;
static pthread_mutex_t g_print_mutex;
static volatile sig_atomic_t g_keep_running = 1;

typedef struct {
    char *buf;
    size_t len;
    size_t capacity;
} ResponseBuffer;

static void signal_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_incoming = size * nmemb;
    ResponseBuffer *resp = (ResponseBuffer *)userp;
    if (resp->len < resp->capacity) {
        size_t available = resp->capacity - resp->len;
        size_t to_copy = (total_incoming < available) ? total_incoming : available;
        memcpy(resp->buf + resp->len, contents, to_copy);
        resp->len += to_copy;
        resp->buf[resp->len] = '\0';
    }
    return total_incoming;
}

#ifdef _WIN32
static unsigned __stdcall worker_thread(void *arg)
#else
static void *worker_thread(void *arg)
#endif
{
    (void)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    ResponseBuffer resp;
    resp.capacity = g_max_see_bytes;
    resp.buf = (char *)malloc(resp.capacity + 1);
    if (!resp.buf) {
        curl_easy_cleanup(curl);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    while (g_keep_running) {
        uint64_t task_idx;
        pthread_mutex_lock(&g_task_mutex);
        if (g_current_task_idx >= g_total_tasks) {
            pthread_mutex_unlock(&g_task_mutex);
            break;
        }
        task_idx = g_current_task_idx++;
        pthread_mutex_unlock(&g_task_mutex);
        uint64_t host_offset = task_idx / 2;
        int port = (task_idx % 2 == 0) ? 80 : 443;
        uint32_t ip_int = g_start_ip + (uint32_t)host_offset;

        char ip_str[32];
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                 (ip_int >> 24) & 0xFF,
                 (ip_int >> 16) & 0xFF,
                 (ip_int >> 8) & 0xFF,
                 ip_int & 0xFF);

        char url[128];
        if (port == 80) {
            snprintf(url, sizeof(url), "http://%s:80/", ip_str);
        } else {
            snprintf(url, sizeof(url), "https://%s:443/", ip_str);
        }
        resp.len = 0;
        resp.buf[0] = '\0';
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, g_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, g_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (lslanpage/2.0)");
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (res == CURLE_OK && resp.len > 0) {
            pthread_mutex_lock(&g_print_mutex);
            printf("================================================================\n");
            printf("[SUCCESS] URL: %s\n", url);
            printf("[STATUS ] IP: %s | Port: %d | HTTP Status: %ld | Size: %zu bytes\n", 
                   ip_str, port, http_code, resp.len);
            printf("----------------------- PAGE CONTENT ---------------------------\n");
            printf("%s\n", resp.buf);
            printf("================================================================\n\n");
            fflush(stdout);
            pthread_mutex_unlock(&g_print_mutex);
        }
    }

    free(resp.buf);
    curl_easy_cleanup(curl);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int parse_cidr(const char *cidr) {
    char ip_str[64];
    int prefix = 0;
    if (sscanf(cidr, "%63[^/]/%d", ip_str, &prefix) != 2) {
        return -1;
    }
    if (prefix < 0 || prefix > 32) {
        return -1;
    }

    unsigned int b1, b2, b3, b4;
    if (sscanf(ip_str, "%u.%u.%u.%u", &b1, &b2, &b3, &b4) != 4) {
        return -1;
    }
    if (b1 > 255 || b2 > 255 || b3 > 255 || b4 > 255) {
        return -1;
    }

    uint32_t base_ip = (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
    uint32_t mask = (prefix == 0) ? 0 : (0xFFFFFFFFU << (32 - prefix));

    g_start_ip = base_ip & mask;
    g_end_ip = (base_ip & mask) | (~mask);
    g_total_hosts = (uint64_t)g_end_ip - g_start_ip + 1;
    g_total_tasks = g_total_hosts * 2; /* 80 与 443 */

    return 0;
}

static void print_usage(const char *prog_name) {
    printf("用法: %s -ip <CIDR> -th <并发线程> -wait <超时毫秒> -sleep <间隔秒数> -see <截断字节数>\n\n", prog_name);
    printf("必选参数:\n");
    printf("  -ip    目标网段 (例如: 192.168.10.0/24, 192.0.0.0/16, 172.0.0.0/8)\n");
    printf("  -th    并发请求线程数 (推荐: 12 - 50)\n");
    printf("  -wait  单目标连接/读取超时时间 (毫秒, 例如: 2200)\n");
    printf("  -sleep 单轮全扫描完成后的休眠时间 (秒, 例如: 36000)\n");
    printf("  -see   单页面终端打印截断上限 (字节, 例如: 2048)\n\n");
    printf("示例:\n");
    printf("  %s -ip 192.0.0.0/24 -th 12 -wait 2200 -sleep 36000 -see 2048\n", prog_name);
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    setlocale(LC_ALL, "");
#endif

    char *cidr_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-ip") == 0 && i + 1 < argc) {
            cidr_arg = argv[++i];
        } else if (strcmp(argv[i], "-th") == 0 && i + 1 < argc) {
            g_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-wait") == 0 && i + 1 < argc) {
            g_timeout_ms = atol(argv[++i]);
        } else if (strcmp(argv[i], "-sleep") == 0 && i + 1 < argc) {
            g_sleep_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-see") == 0 && i + 1 < argc) {
            g_max_see_bytes = (size_t)atol(argv[++i]);
        }
    }

    if (!cidr_arg || g_threads <= 0 || g_timeout_ms <= 0 || g_sleep_interval <= 0 || g_max_see_bytes == 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (parse_cidr(cidr_arg) != 0) {
        fprintf(stderr, "[ERROR] 无效的 CIDR 格式: %s\n", cidr_arg);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    pthread_mutex_init(&g_task_mutex, NULL);
    pthread_mutex_init(&g_print_mutex, NULL);
    curl_global_init(CURL_GLOBAL_ALL);
    printf("[*] 网段: %s (总主机数: %llu, 总任务数: %llu)\n", cidr_arg, (unsigned long long)g_total_hosts, (unsigned long long)g_total_tasks);
    printf("[*] 配置: 线程数=%d, 超时=%ldms, 周期=%ds, 截断=%zubytes\n", 
           g_threads, g_timeout_ms, g_sleep_interval, g_max_see_bytes);

    while (g_keep_running) {
        printf("\n[=== 启动扫描轮次 ===]\n");
        g_current_task_idx = 0;

#ifdef _WIN32
        HANDLE *threads = (HANDLE *)malloc(sizeof(HANDLE) * g_threads);
        int active_threads = 0;
        for (int i = 0; i < g_threads; i++) {
            threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
            if (threads[i] != 0) {
                active_threads++;
            }
        }
        for (int i = 0; i < active_threads; i++) {
            WaitForSingleObject(threads[i], INFINITE);
            CloseHandle(threads[i]);
        }
        free(threads);
#else
        pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * g_threads);
        int active_threads = 0;
        for (int i = 0; i < g_threads; i++) {
            if (pthread_create(&threads[i], NULL, worker_thread, NULL) == 0) {
                active_threads++;
            }
        }
        for (int i = 0; i < active_threads; i++) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
#endif

        if (!g_keep_running) break;
        printf("[=== 轮次结束，等待下一次循环 ===]\n");
        for (int s = 0; s < g_sleep_interval && g_keep_running; s++) {
            sleep_sec(1);
        }
    }
    curl_global_cleanup();
    pthread_mutex_destroy(&g_task_mutex);
    pthread_mutex_destroy(&g_print_mutex);
    return 0;
}
