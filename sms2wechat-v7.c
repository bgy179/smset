/********
 * 编译说明 (Windows MinGW / MSYS2 环境下):
 * gcc sms_wechat_router_gateway.c -o sms_wechat_router_gateway.exe -I./mysql/include -I./curl/include -L./mysql/lib -L./curl/lib -lmysql -lcurl -lws2_32 -lpthread -lregex
 ********/

#define _XOPEN_SOURCE 700
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <regex.h>
#include <pthread.h> 

#include <mysql/mysql.h>
#include <curl/curl.h>

// ===================== 配置区 =====================
#define SERVICE_NAME_A     "SmsWechatRouterGateway"
#define SERVICE_DISPLAY    "短信接收及动态路由微信网关服务"
#define SERVICE_DESC       "采集Air780短信，根据IP内存路由表分发至多个微信群，含断线重试补偿"
#define BAUD_RATE          CBR_115200
#define DB_PORT         3306

#define POLL_INTERVAL                3000  /* 保险串口轮询间隔 (毫秒) */

#define HASH_BUCKET_COUNT    1024
#define MAX_WXID_PER_QUERY   64   
#define RESP_BUF_INIT_SIZE   1024

#define CONFIG_FILENAME      "config.ini"
#define LOG_FILENAME         "sms_service.log"
#define BUF_LEN            1024
#define AT_BUF             (128 * 1024)
#define MAX_COM_NUM        6     
#define AT_TEST_TIMEOUT    400
#define SEGMENT_TIMEOUT_SEC 120  

#define DEFAULT_WECHAT_SEND_API_URL  "http://127.0.0.1:8080/api/sendtxtmsg"
#define DEFAULT_FALLBACK_WXID        "21167291234@chatroom"
#define DEFAULT_DB_HOST              "127.0.0.1"
#define DEFAULT_DB_USER              "root"
#define DEFAULT_DB_PASSWORD          ""
#define DEFAULT_DB_NAME              "alerts"
#define DEFAULT_LOG_PATH             ".\\sms_service.log"
#define DEFAULT_CACHE_REFRESH_INTERVAL_SEC 300

// ===================== 数据结构定义 =====================
typedef struct {
    bool is_concat;
    bool is_16bit_ref;      
    uint16_t ref_num;       
    uint8_t total_parts;
    uint8_t part_num;
    char sender[32];
    uint8_t msg_data[1400]; 
    size_t data_len;
    uint8_t dcs;            
} SMSFrame;

typedef struct SMSPartNode {
    uint8_t part_num;
    uint8_t *data;
    size_t data_len;
    struct SMSPartNode *next;
} SMSPartNode;

typedef struct PendingSMS {
    char sender[32];
    uint16_t ref_num;
    bool is_16bit_ref;
    uint8_t total_parts;
    uint8_t arrived_count;
    time_t first_seen;
    SMSPartNode *parts_head;
    struct PendingSMS *next;
    struct PendingSMS *prev;
} PendingSMS;

typedef struct wxid_node {
    char wxid[128];
    char nickname[256];
    char tenantname[128];
    struct wxid_node *next;
} wxid_node_t;

typedef struct cache_entry {
    char tenantip[64];
    wxid_node_t *wxid_list;      
    struct cache_entry *next;    
} cache_entry_t;

typedef struct {
    cache_entry_t *buckets[HASH_BUCKET_COUNT];
} hash_table_t;

typedef struct {
    char wxids[MAX_WXID_PER_QUERY][128];
    int count;
} wxid_result_t;

typedef struct {
    char* buffer;
    size_t len;
} ResponseData;

typedef struct {
    char wechat_send_api_url[256];
    char fallback_wxid[128];
    char db_host[128];
    char db_user[64];
    char db_password[128];
    char db_name[64];
    char log_path[260];
    int cache_refresh_interval_sec;
} AppConfig;

// ===================== 全局变量 =====================
HANDLE hSerial = INVALID_HANDLE_VALUE;
WCHAR g_ValidComPort[32] = {0}; 
MYSQL* mysql = NULL;
FILE* log_fp = NULL;

SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_hStatusHandle = NULL;
HANDLE g_hStopEvent = NULL;

PendingSMS *pending_queue_head = NULL;
bool g_bDebugMode = false;

static hash_table_t *g_cache = NULL;
static pthread_rwlock_t g_cache_lock = PTHREAD_RWLOCK_INITIALIZER;
static AppConfig g_cfg;
static char g_exe_dir[MAX_PATH] = {0};

static void config_set_defaults(void) {
    snprintf(g_cfg.wechat_send_api_url, sizeof(g_cfg.wechat_send_api_url), "%s", DEFAULT_WECHAT_SEND_API_URL);
    snprintf(g_cfg.fallback_wxid, sizeof(g_cfg.fallback_wxid), "%s", DEFAULT_FALLBACK_WXID);
    snprintf(g_cfg.db_host, sizeof(g_cfg.db_host), "%s", DEFAULT_DB_HOST);
    snprintf(g_cfg.db_user, sizeof(g_cfg.db_user), "%s", DEFAULT_DB_USER);
    snprintf(g_cfg.db_password, sizeof(g_cfg.db_password), "%s", DEFAULT_DB_PASSWORD);
    snprintf(g_cfg.db_name, sizeof(g_cfg.db_name), "%s", DEFAULT_DB_NAME);
    snprintf(g_cfg.log_path, sizeof(g_cfg.log_path), "%s", DEFAULT_LOG_PATH);
    g_cfg.cache_refresh_interval_sec = DEFAULT_CACHE_REFRESH_INTERVAL_SEC;
}

static void init_exe_dir(void) {
    DWORD len = GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    if (len == 0 || len >= sizeof(g_exe_dir)) {
        strcpy(g_exe_dir, ".");
        return;
    }

    char *slash = strrchr(g_exe_dir, '\\');
    if (slash) {
        *slash = '\0';
    } else {
        strcpy(g_exe_dir, ".");
    }
}

static void build_path_in_exe_dir(char *out, size_t out_size, const char *name) {
    if (!out || out_size == 0 || !name) return;
    snprintf(out, out_size, "%s\\%s", g_exe_dir[0] ? g_exe_dir : ".", name);
}

static void trim_whitespace(char *s) {
    if (!s) return;

    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static void config_apply_kv(const char *key, const char *value) {
    if (!key || !value) return;

    if (_stricmp(key, "WECHAT_SEND_API_URL") == 0) {
        snprintf(g_cfg.wechat_send_api_url, sizeof(g_cfg.wechat_send_api_url), "%s", value);
    } else if (_stricmp(key, "FALLBACK_WXID") == 0) {
        snprintf(g_cfg.fallback_wxid, sizeof(g_cfg.fallback_wxid), "%s", value);
    } else if (_stricmp(key, "DB_HOST") == 0) {
        snprintf(g_cfg.db_host, sizeof(g_cfg.db_host), "%s", value);
    } else if (_stricmp(key, "DB_USER") == 0 || _stricmp(key, "DBUSER") == 0) {
        snprintf(g_cfg.db_user, sizeof(g_cfg.db_user), "%s", value);
    } else if (_stricmp(key, "DB_PASSWORD") == 0 || _stricmp(key, "DBPASSWORD") == 0) {
        snprintf(g_cfg.db_password, sizeof(g_cfg.db_password), "%s", value);
    } else if (_stricmp(key, "DB_NAME") == 0 || _stricmp(key, "DBNAME") == 0) {
        snprintf(g_cfg.db_name, sizeof(g_cfg.db_name), "%s", value);
    } else if (_stricmp(key, "LOG_PATH") == 0) {
        snprintf(g_cfg.log_path, sizeof(g_cfg.log_path), "%s", value);
    } else if (_stricmp(key, "CACHE_REFRESH_INTERVAL_SEC") == 0 || _stricmp(key, "CACH_REFRESH_INTERVAL") == 0) {
        int sec = atoi(value);
        if (sec > 0) g_cfg.cache_refresh_interval_sec = sec;
    }
}

static void load_config_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim_whitespace(key);
        trim_whitespace(value);

        if (key[0] == '\0') continue;
        config_apply_kv(key, value);
    }

    fclose(fp);
}

static void load_config_from_exe_dir(void) {
    char config_path[MAX_PATH];
    build_path_in_exe_dir(config_path, sizeof(config_path), CONFIG_FILENAME);
    load_config_file(config_path);
}

static BOOL SerialWriteAll(HANDLE h, const char *buf, DWORD len) {
    if (h == INVALID_HANDLE_VALUE || buf == NULL) return FALSE;

    DWORD sent_total = 0;
    while (sent_total < len) {
        DWORD sent_now = 0;
        if (!WriteFile(h, buf + sent_total, len - sent_total, &sent_now, NULL))
            return FALSE;
        if (sent_now == 0)
            return FALSE;
        sent_total += sent_now;
    }
    return TRUE;
}

/*
 * Read and accumulate serial bytes until the line is idle for idle_ms, or
 * until total_timeout_ms elapses. Returns TRUE if any byte was received.
 */
static BOOL SerialCollectUntilQuiet(HANDLE h, char *out, size_t out_cap,
                                    DWORD total_timeout_ms, DWORD idle_ms) {
    if (h == INVALID_HANDLE_VALUE || out == NULL || out_cap < 2) return FALSE;

    DWORD start_tick = GetTickCount();
    DWORD last_data_tick = start_tick;
    size_t total = 0;
    BOOL has_data = FALSE;

    out[0] = '\0';

    while (GetTickCount() - start_tick < total_timeout_ms) {
        char chunk[BUF_LEN];
        DWORD got = 0;

        if (ReadFile(h, chunk, (DWORD)(sizeof(chunk) - 1), &got, NULL) && got > 0) {
            has_data = TRUE;
            last_data_tick = GetTickCount();

            size_t copy = (size_t)got;
            if (total + copy >= out_cap)
                copy = out_cap - total - 1;

            if (copy > 0) {
                memcpy(out + total, chunk, copy);
                total += copy;
                out[total] = '\0';
            }

            if (total >= out_cap - 1)
                break;

            continue;
        }

        if (has_data && (GetTickCount() - last_data_tick >= idle_ms))
            break;

        Sleep(10);
    }

    return has_data;
}

// ===================== 基础工具函数 =====================
void WriteLog(const char* fmt, ...) {
    va_list ap;
    time_t now = time(NULL);
    char timestr[64];
    struct tm *local_tm = localtime(&now);
    if (local_tm) strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", local_tm);
    else strcpy(timestr, "UNKNOWN");

    if (g_bDebugMode) {
        va_start(ap, fmt);
        printf("[%s] [DEBUG] ", timestr);
        vprintf(fmt, ap);
        printf("\n");
        fflush(stdout);
        va_end(ap);
    }

    if (!log_fp) return;
    va_start(ap, fmt);
    fprintf(log_fp, "[%s] ", timestr);
    vfprintf(log_fp, fmt, ap);
    fprintf(log_fp, "\r\n");
    fflush(log_fp);
    va_end(ap);
}

int hexchar_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool hex_to_bytes(const char *hex_str, uint8_t **out_bin, size_t *out_len) {
    size_t hex_len = strlen(hex_str);
    size_t clean_len = 0;
    for(size_t i=0; i<hex_len; i++) { 
        if(hexchar_to_val(hex_str[i]) >= 0) 
        clean_len++; 
    }
    if (clean_len % 2 != 0 || clean_len == 0) return false;
    size_t bin_len = clean_len / 2;
    uint8_t *bin = malloc(bin_len);
    if (!bin) return false;
    size_t j = 0; int high = -1;
    for(size_t i=0; i<hex_len; i++){
        int v = hexchar_to_val(hex_str[i]);
        if(v < 0) continue;
        if(high == -1) high = v;
        else { bin[j++] = (high << 4) | v; high = -1; }
    }
    *out_bin = bin; *out_len = bin_len;
    return true;
}

// 将 GSM 7-bit 压缩编码解码为标准 UTF-8/ASCII 字符串
bool gsm7bit_to_utf8(const uint8_t *src, size_t bit7_char_count, char *dst, size_t dst_max_len, size_t shift_bits) {
    if (bit7_char_count == 0 || dst_max_len == 0) return false;
    if (bit7_char_count >= dst_max_len) bit7_char_count = dst_max_len - 1;

    WriteLog("[GSM7解码明细] 待处理字符数: %zu, 初始位移偏移(shift_bits): %zu", bit7_char_count, shift_bits);
    size_t byte_idx = 0;
    size_t bit_offset = shift_bits; 

    for (size_t i = 0; i < bit7_char_count; i++) {
        byte_idx = (i * 7 + shift_bits) / 8;
        bit_offset = (i * 7 + shift_bits) % 8;

        uint8_t ch = 0;
        if (bit_offset <= 1) {
            ch = (src[byte_idx] >> bit_offset) & 0x7F;
        } else {
            uint8_t low = src[byte_idx] >> bit_offset;
            uint8_t high = src[byte_idx + 1] << (8 - bit_offset);
            ch = (low | high) & 0x7F;
        }
        dst[i] = (char)ch;
    }
    dst[bit7_char_count] = '\0';
    WriteLog("[GSM7解码明细] 还原字符串成功: %s", dst);

    return true;
}

bool ucs2be_to_utf8(const uint8_t *ucs2_src, size_t src_len, char *utf8_dst, size_t dst_max_len) {
    if (src_len % 2 != 0 || src_len == 0) {
        WriteLog("[UCS2解码明细] 错误：数据长度 %zu 不是2的倍数，拒绝转换", src_len);
        return false;
    }
    size_t wlen = src_len / 2;
    WriteLog("[UCS2解码明细] 正在将 %zu 字节的大端序 UCS2 转换为 UTF-8 (宽字符数: %zu)...", src_len, wlen);
    
    wchar_t *wbuf = malloc((wlen + 1) * sizeof(wchar_t));
    if (!wbuf) return false;
    for (size_t i = 0; i < wlen; i++) {
        wbuf[i] = (wchar_t)((ucs2_src[i * 2] << 8) | ucs2_src[i * 2 + 1]);
    }
    wbuf[wlen] = L'\0';
    int req_len = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)wlen, NULL, 0, NULL, NULL);
    if (req_len <= 0 || (size_t)req_len >= dst_max_len) 
    { 
        free(wbuf); 
        WriteLog("[UCS2解码明细] 错误：转换所需的缓冲区长度 %d 溢出", req_len);
        return false; 
    }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)wlen, utf8_dst, (int)dst_max_len - 1, NULL, NULL);
    utf8_dst[req_len] = '\0'; 
    WriteLog("[UCS2解码明细] 解码成功。文本: %s", utf8_dst);
    free(wbuf);
    return true;
}

void decode_semi_octet(const uint8_t *src, size_t num_digits, char *dst) {
    size_t dst_idx = 0;
    for (size_t i = 0; dst_idx < num_digits; i++) {
        uint8_t low = src[i] & 0x0F; uint8_t high = (src[i] & 0xF0) >> 4;
        if (low < 10) dst[dst_idx++] = '0' + low;
        if (high < 10 && dst_idx < num_digits) dst[dst_idx++] = '0' + high;
    }
    dst[dst_idx] = '\0';
}

// ===================== 内存缓存核心控制逻辑 =====================
static unsigned long hash_str(const char *s) {
    unsigned long h = 5381; int c;
    while ((c = (unsigned char)*s++)) { h = ((h << 5) + h) + c; }
    return h;
}

static hash_table_t *hash_table_create(void) {
    return (hash_table_t *)calloc(1, sizeof(hash_table_t));
}

static cache_entry_t *bucket_find_entry(cache_entry_t *bucket_head, const char *tenantip) {
    cache_entry_t *e = bucket_head;
    while (e) {
        if (strcmp(e->tenantip, tenantip) == 0) return e;
        e = e->next;
    }
    return NULL;
}

static void hash_table_insert(hash_table_t *t, const char *tenantip, const char *tenantname, const char *wxid, const char *nickname) {
    unsigned long idx = hash_str(tenantip) % HASH_BUCKET_COUNT;
    cache_entry_t *e = bucket_find_entry(t->buckets[idx], tenantip);
    if (!e) {
        e = (cache_entry_t *)calloc(1, sizeof(cache_entry_t));
        snprintf(e->tenantip, sizeof(e->tenantip), "%s", tenantip);
        e->next = t->buckets[idx]; t->buckets[idx] = e;
    }
    for (wxid_node_t *n = e->wxid_list; n; n = n->next) {
        if (strcmp(n->wxid, wxid) == 0) return;
    }
    wxid_node_t *node = (wxid_node_t *)malloc(sizeof(wxid_node_t));
    snprintf(node->wxid, sizeof(node->wxid), "%s", wxid);
    snprintf(node->nickname, sizeof(node->nickname), "%s", nickname ? nickname : "");
    snprintf(node->tenantname, sizeof(node->tenantname), "%s", tenantname ? tenantname : "");
    node->next = e->wxid_list; e->wxid_list = node;
}

static cache_entry_t *hash_table_find(hash_table_t *t, const char *tenantip) {
    if (!t) return NULL;
    return bucket_find_entry(t->buckets[hash_str(tenantip) % HASH_BUCKET_COUNT], tenantip);
}

static void hash_table_free(hash_table_t *t) {
    if (!t) return;
    for (int i = 0; i < HASH_BUCKET_COUNT; i++) {
        cache_entry_t *e = t->buckets[i];
        while (e) {
            cache_entry_t *next_e = e->next; wxid_node_t *n = e->wxid_list;
            while (n) { wxid_node_t *next_n = n->next; free(n); n = next_n; }
            free(e); e = next_e;
        }
    }
    free(t);
}

static void copy_entry_to_result(cache_entry_t *e, wxid_result_t *result) {
    result->count = 0; if (!e) return;
    for (wxid_node_t *n = e->wxid_list; n && result->count < MAX_WXID_PER_QUERY; n = n->next) {
        snprintf(result->wxids[result->count], sizeof(result->wxids[0]), "%s", n->wxid);
        result->count++;
    }
}

// ===================== 数据库连接与路由数据加载 =====================
static MYSQL *db_connect(void) {
    MYSQL *conn = mysql_init(NULL);
    if (!conn) return NULL;
        
    int ssl_mode = 0;
    mysql_options(conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &ssl_mode);
    if (!mysql_real_connect(conn, g_cfg.db_host, g_cfg.db_user, g_cfg.db_password, g_cfg.db_name, DB_PORT, NULL, 0)) {
        WriteLog("[数据库] 连接失败，宿主机: %s, 错误原因: %s", g_cfg.db_host, mysql_error(conn));
        mysql_close(conn); return NULL;
    }
    mysql_set_character_set(conn, "utf8mb4");
    return conn;
}

static int cache_full_refresh(void) {
    WriteLog("[路由缓存] 开始加载/刷新全量路由表规则...");
    MYSQL *conn = db_connect();
    if (!conn) {
        WriteLog("[路由缓存] 刷新终止：由于数据库连不上，保持旧缓存不变");
        return -1;
    }
    const char *sql = "SELECT t.tenantname, t.tenantip, c.wxid, c.nickname "
                      "FROM wzwmonitor.paas_cluster_tenantname_ip t "
                      "JOIN wzwmonitor.wechat_chatroom c ON t.tenantname = c.tenantname";
    if (mysql_query(conn, sql) != 0) { 
        WriteLog("[路由缓存] SQL执行错误: %s", mysql_error(conn));
        mysql_close(conn); return -1; 
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) { mysql_close(conn); return -1; }

    hash_table_t *new_table = hash_table_create();
    MYSQL_ROW row;
    int count = 0;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (row[1] && row[2] && row[1][0] != '\0' && row[2][0] != '\0') {
            hash_table_insert(new_table, row[1], row[0], row[2], row[3]);
            count++;
        }
    }
    mysql_free_result(res); mysql_close(conn);

    pthread_rwlock_wrlock(&g_cache_lock);
    hash_table_t *old_table = g_cache; g_cache = new_table;
    pthread_rwlock_unlock(&g_cache_lock);
    hash_table_free(old_table);
    
    WriteLog("[路由缓存] 全量刷新同步完毕，当前共载入 %d 条映射链路", count);
    return 0;
}

static int query_db_by_ip(const char *tenantip, wxid_result_t *result) {
    result->count = 0;
    WriteLog("[路由查找] 缓存未击中，触发数据库降级穿透巡检 IP: %s", tenantip);
    MYSQL *conn = db_connect();
    if (!conn) return -1;
    char sql[512], escaped_ip[256];
    mysql_real_escape_string(conn, escaped_ip, tenantip, strlen(tenantip));
    snprintf(sql, sizeof(sql),
        "SELECT t.tenantname, t.tenantip, c.wxid, c.nickname "
        "FROM wzwmonitor.paas_cluster_tenantname_ip t "
        "JOIN wzwmonitor.wechat_chatroom c ON t.tenantname = c.tenantname "
        "WHERE t.tenantip = '%s'", escaped_ip);

    if (mysql_query(conn, sql) != 0) { mysql_close(conn); return -1; }
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) { mysql_close(conn); return -1; }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != NULL) {
        if (!row[2] || row[2][0] == '\0') continue;
        if (result->count < MAX_WXID_PER_QUERY) {
            snprintf(result->wxids[result->count], sizeof(result->wxids[0]), "%s", row[2]);
            result->count++;
        }
        pthread_rwlock_wrlock(&g_cache_lock);
        if (g_cache) hash_table_insert(g_cache, row[1], row[0], row[2], row[3]);
        pthread_rwlock_unlock(&g_cache_lock);
    }
    mysql_free_result(res); mysql_close(conn);
    WriteLog("[路由查找] 数据库穿透查询完毕，IP: %s 最终映射到 %d 个群组", tenantip, result->count);
    return result->count > 0 ? 0 : -1;
}

static int lookup_wxid_by_tenantip(const char *tenantip, wxid_result_t *result) {
    result->count = 0;
    pthread_rwlock_rdlock(&g_cache_lock);
    cache_entry_t *e = hash_table_find(g_cache, tenantip);
    if (e) copy_entry_to_result(e, result);
    pthread_rwlock_unlock(&g_cache_lock);
    
    if (result->count > 0) {
        WriteLog("[路由查找] 内存高效击中！IP: %s 映射至 %d 个微信群", tenantip, result->count);
        return 0;
    }
    return query_db_by_ip(tenantip, result);
}

DWORD WINAPI CacheRefreshThreadProc(LPVOID lpParam) {
    (void)lpParam;
    WriteLog("[后台线程] 5分钟定期路由全量刷新线程挂载就绪");
    for (;;) {
        Sleep((DWORD)g_cfg.cache_refresh_interval_sec * 1000);
        cache_full_refresh();
    }
    return 0;
}

// ===================== 从短信文本中提取 IP =====================
static int extract_tenantip_from_message(const char *msg, char *out_ip, size_t out_len) {
    static const char *pattern = "<IP>([0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3})<IP>";
    regex_t re; 
    regmatch_t match[2]; 
    int ret = -1;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return -1;
    if (regexec(&re, msg, 2, match, 0) == 0) {
        int len = match[1].rm_eo - match[1].rm_so;
        if (len > 0 && (size_t)len < out_len) {
            memcpy(out_ip, msg + match[1].rm_so, len); 
            out_ip[len] = '\0'; 
            ret = 0;
            WriteLog("[正则提纯] 从短信文本成功匹配出目标IP标记: %s", out_ip);
        }
    }
    regfree(&re); return ret;
}

// ===================== JSON 字符串转义函数 =====================
static void json_escape_string(const char *src, char *dst, size_t dst_size) {
    size_t dst_idx = 0;
    for (size_t i = 0; src[i] != '\0' && dst_idx < dst_size - 2; i++) {
        switch (src[i]) {
            case '\\': dst[dst_idx++] = '\\'; dst[dst_idx++] = '\\'; break;
            case '"':  dst[dst_idx++] = '\\'; dst[dst_idx++] = '"'; break;
            case '\n': dst[dst_idx++] = '\\'; dst[dst_idx++] = 'n'; break;
            case '\r': dst[dst_idx++] = '\\'; dst[dst_idx++] = 'r'; break;
            case '\t': dst[dst_idx++] = '\\'; dst[dst_idx++] = 't'; break;
            default:   dst[dst_idx++] = src[i]; break;
        }
    }
    dst[dst_idx] = '\0';
}

// ===================== 微信接口发送驱动 (带5秒超时) =====================
static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb; 
    ResponseData *resp = (ResponseData *)userp;
    char *new_buf = realloc(resp->buffer, resp->len + real_size + 1);
    if (!new_buf) return 0;
    resp->buffer = new_buf; 
    memcpy(resp->buffer + resp->len, contents, real_size);
    resp->len += real_size; 
    resp->buffer[resp->len] = '\0';
    return real_size;
}

static int send_wechat_message(const char *wxid, const char *content) {
    CURL *curl = curl_easy_init();
    if (!curl || !content) return -1;

    char new_content[4096]; 
    const char *marker = strstr(content, "=>| ");
    const char *ptr_from = marker ? (marker + 4) : content;
    char *ptr_to = new_content;

    if (!marker) {
        WriteLog("[内容清洗] 文本中未检测到 '=>| ' 标记，复制起点为文本开头");
    }

    while(*ptr_from != '\0' && ptr_to < new_content + sizeof(new_content) - 1) {
        if(*ptr_from == '|' && *(ptr_from + 1) == ' ') {
            ptr_from += 2;
        } else { 
            *ptr_to = *ptr_from; 
            ptr_to++; 
            ptr_from++;
        }
    }
    *ptr_to = '\0';

    char escaped_content[8192];
    json_escape_string(new_content, escaped_content, sizeof(escaped_content));

    char body[8192];
    snprintf(body, sizeof(body), "{\"wxid\":\"%s\",\"content\":\"%s\"}", wxid, escaped_content);
    WriteLog("msg to go: %s\n", body);
	
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    ResponseData resp_data = { malloc(RESP_BUF_INIT_SIZE), 0 };
    if (!resp_data.buffer) { 
        curl_slist_free_all(headers); 
        curl_easy_cleanup(curl); 
        return -1; 
    }
    resp_data.buffer[0] = '\0';
    
    curl_easy_setopt(curl, CURLOPT_URL, g_cfg.wechat_send_api_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); 

    WriteLog("[HTTP客户端] 正在向微信网关群 %s 推送Payload数据...", wxid);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0; 
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers); 
    curl_easy_cleanup(curl);

    int ret_status = -1;
    if (rc == CURLE_OK && http_code >= 200 && http_code < 300) {
        if (strstr(resp_data.buffer, "\"msg\":\"success\"") != NULL) {
            ret_status = 0;
        } else {
            WriteLog("[HTTP异常] 网关应答包未包含success标志。Body: %s", resp_data.buffer);
        }
    } else {
        WriteLog("[HTTP错误] CURL执行状态错误码: %d, HTTP状态码: %ld", rc, http_code);
    }
    free(resp_data.buffer); return ret_status;
}

// ===================== 核心分发：纯异步落库 =====================
void dispatch_decoded_sms(const char *sender, uint16_t ref_num, bool is_16bit, const char *content_utf8) {
    char ip[64];
    wxid_result_t route_result;
    route_result.count = 0;

    WriteLog("[业务路由] 开始介入清洗已还原的完整可读文本内容...");
    if (extract_tenantip_from_message(content_utf8, ip, sizeof(ip)) != 0) {
        WriteLog("[业务路由] 警告：文本内未匹配出有效的<IP>闭合标记，强制归入系统兜底微信群: %s", g_cfg.fallback_wxid);
        strcpy(route_result.wxids[0], g_cfg.fallback_wxid); route_result.count = 1;
    } else {
        if (lookup_wxid_by_tenantip(ip, &route_result) != 0 || route_result.count == 0) {
            WriteLog("[业务路由] 警告：提取出IP %s 但在现有映射字典中均无匹配项，强制进入兜底群", ip);
            strcpy(route_result.wxids[0], g_cfg.fallback_wxid); route_result.count = 1;
        }
    }

    for (int i = 0; i < route_result.count; i++) {
        int inserted_id = -1;
        MYSQL_STMT *stmt = mysql_stmt_init(mysql);
        if (stmt) {
            const char *sql = "INSERT INTO decoded_sms (sender, ref_num, is_16bit, message_content, target_wxid, send_status) VALUES (?, ?, ?, ?, ?, 0);";
            if (mysql_stmt_prepare(stmt, sql, strlen(sql)) == 0) {
                MYSQL_BIND bind[5]; 
                memset(bind, 0, sizeof(bind));
                unsigned long sender_len = strlen(sender);
                unsigned long content_len = strlen(content_utf8);
                unsigned long wxid_len = strlen(route_result.wxids[i]);
                int is_16bit_int = is_16bit ? 1 : 0;

                bind[0].buffer_type = MYSQL_TYPE_STRING; 
                bind[0].buffer = (char*)sender; 
                bind[0].buffer_length = sender_len;
                bind[1].buffer_type = MYSQL_TYPE_SHORT; 
                bind[1].buffer = &ref_num;
                bind[2].buffer_type = MYSQL_TYPE_TINY; 
                bind[2].buffer = &is_16bit_int;
                bind[3].buffer_type = MYSQL_TYPE_STRING; 
                bind[3].buffer = (char*)content_utf8; 
                bind[3].buffer_length = content_len;
                bind[4].buffer_type = MYSQL_TYPE_STRING; 
                bind[4].buffer = route_result.wxids[i]; 
                bind[4].buffer_length = wxid_len;

                mysql_stmt_bind_param(stmt, bind);
                if(mysql_stmt_execute(stmt) == 0) {
                    inserted_id = (int)mysql_insert_id(mysql);
                } else {
                    WriteLog("[预编译异常] 写入明文表 decoded_sms 失败: %s", mysql_stmt_error(stmt));
                }
            }
            mysql_stmt_close(stmt);
        }

        if (inserted_id != -1) {
            WriteLog("[落库回执] 明文解析数据已成功录入系统底层 (表ID: %d) -> 拟下发群: %s", inserted_id, route_result.wxids[i]);
        }
    }
}

// ===================== 独立的后台异步微信发送线程 =====================
void* WeChatAsyncPushThreadProc(void* lpParam) {
    (void)lpParam;
    WriteLog("[后台线程] 异步微信下发推送的主控消费引擎已成功挂载运行");
    
    // 等待3秒让系统完全初始化
    Sleep(3000);
    WriteLog("[后台线程] 初始化等待完成，开始进入主消费循环");

    while (1) {
        if (!g_bDebugMode && WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        MYSQL *async_mysql = db_connect();
        if (!async_mysql) {
            WriteLog("[后台线程] 数据库连接失败，2秒后重试");
            Sleep(2000); 
            continue;
        }

        // WriteLog("[后台线程] 扫描待发送消息队列...");
        const char *sql = "SELECT id, target_wxid, message_content FROM decoded_sms WHERE send_status = 0 ORDER BY id ASC LIMIT 50;";
        if (mysql_query(async_mysql, sql) == 0) {
            MYSQL_RES *res = mysql_store_result(async_mysql);
            if (res) {
                int row_count = 0;
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    row_count++;
                    int id = atoi(row[0]); 
                    const char *target_wxid = row[1]; 
                    const char *content = row[2];
                    
                    WriteLog("[下发队列] 扫到待发送任务 (ID: %d), 提取接收群: %s", id, target_wxid);
                    if (send_wechat_message(target_wxid, content) == 0) {
                        WriteLog("[下发成功] 目标微信群已安全接收 (明文数据表ID: %d)", id);
                        char up_sql[128]; 
                        snprintf(up_sql, sizeof(up_sql), "UPDATE decoded_sms SET send_status=1 WHERE id=%d", id);
                        mysql_query(async_mysql, up_sql);
                    } else {
                        WriteLog("[下发阻断] 推送遭遇故障（已触发布局挂起重试策略机制，ID: %d）", id);
                        break; 
                    }
                }
                mysql_free_result(res);
                if (row_count == 0) {
                    ; // WriteLog("[后台线程] 队列为空，无待发送消息");
                }
            } else {
                WriteLog("[后台线程] mysql_store_result 返回 NULL");
            }
        } else {
            WriteLog("[后台线程] 数据库查询失败: %s", mysql_error(async_mysql));
        }
        
        mysql_close(async_mysql);
        Sleep(3000); 
    }
    
    WriteLog("[后台线程] 异步推送微信服务消费线程已安全终止注销。");
    return NULL;
}

// ===================== 🛠️ 深度细化的 PDU 协议解包层 =====================
bool parse_raw_pdu(const char *raw_hex, SMSFrame *frame) {
    if (!raw_hex || !frame) return false;
    memset(frame, 0, sizeof(SMSFrame)); 
    uint8_t *bin = NULL; 
    size_t bin_len = 0;
    
    WriteLog("[PDU剖析] >>>>>>>>>> 开始对原始16进制进行精细化字节级解构 <<<<<<<<<<");
    WriteLog("[PDU剖析] 原始 HEX 字符串: %s", raw_hex);

    if (!hex_to_bytes(raw_hex, &bin, &bin_len)) {
        WriteLog("[PDU剖析] [错误] 无法将16进制输入文本转换为标准二进制流");
        return false;
    }
    WriteLog("[PDU剖析] 成功转换为二进制字节数组，总长度: %zu 字节", bin_len);
    if (bin_len < 2) { free(bin); return false; }
    
    size_t pos = 0; 
    
    // 1. SCA 段解析
    uint8_t sca_len = bin[pos++];   
    WriteLog("[PDU剖析] [1/9 字段] 短信中心(SCA)长度字节: 0x%02X (%d 字节)", sca_len, sca_len);
    if (pos + sca_len >= bin_len) { 
        WriteLog("[PDU剖析] [边界错误] SCA长度超出总包范围"); 
        free(bin); return false; 
    }
    if (sca_len > 0) {
        char sca_num[32] = {0};
        uint8_t sca_type = bin[pos];
        decode_semi_octet(&bin[pos + 1], (sca_len - 1) * 2, sca_num);
        WriteLog("[PDU剖析] -> SCA 类型: 0x%02X, SCA 号码: %s", sca_type, sca_num);
    }
    pos += sca_len; 

    // 2. PDU-Type 解析
    uint8_t first_octet = bin[pos++];  
    bool has_udh = (first_octet & 0x40) ? true : false;  
    WriteLog("[PDU剖析] [2/9 字段] PDU-Type / First-Octet: 0x%02X (二进制位 6 UDH 标志: %s)", 
             first_octet, has_udh ? "1 [有UDH头，代表长短信或多媒体]" : "0 [无UDH头，单片短信]");
    if (pos >= bin_len) { free(bin); return false; }
    
    // 3. OA 发送方地址解析
    uint8_t oa_digits = bin[pos++];  
    uint8_t oa_bytes = (oa_digits + 1) / 2; 
    uint8_t oa_type = bin[pos++];
    WriteLog("[PDU剖析] [3/9 字段] 发送方(OA)长度: %d 位(Digits), 占用空间: %d 字节, OA类型: 0x%02X", 
             oa_digits, oa_bytes, oa_type);
    
    if (pos + oa_bytes >= bin_len) { 
        WriteLog("[PDU剖析] [边界错误] OA实体号码区域溢出"); 
        free(bin); return false; 
    }
    
    if (oa_digits > 0 && oa_digits < 32) {
        decode_semi_octet(&bin[pos], oa_digits, frame->sender);
        WriteLog("[PDU剖析] -> 解密出的发送方手机号(OA): %s", frame->sender);
    } else {
        strcpy(frame->sender, "UNKNOWN");
        WriteLog("[PDU剖析] -> 发送方长度异常，强标记为 UNKNOWN");
    }
    pos += oa_bytes; 

    // 4. PID 解析
    uint8_t pid = bin[pos++]; 
    WriteLog("[PDU剖析] [4/9 字段] 协议标识(PID): 0x%02X", pid);
    if (pos >= bin_len) { free(bin); return false; }
    
    // 5. DCS 编码解析
    frame->dcs = bin[pos++];  
    bool is_7bit = ((frame->dcs & 0x0C) == 0x00);
    bool is_8bit = ((frame->dcs & 0x0C) == 0x04);
    bool is_ucs2 = ((frame->dcs & 0x0C) == 0x08);
    WriteLog("[PDU剖析] [5/9 字段] 编码方案(DCS): 0x%02X -> 判定对应格式: %s", 
             frame->dcs, is_7bit ? "GSM 7-bit (英文字符压缩)" : (is_ucs2 ? "UCS2-BE (双字节中文)" : "8-bit/未知"));
    if (pos + 7 >= bin_len) { free(bin); return false; }
    
    // 6. SCTS 时间戳解析
    char scts_time[32] = {0};
    decode_semi_octet(&bin[pos], 14, scts_time);
    WriteLog("[PDU剖析] [6/9 字段] 服务中心时间戳(SCTS)原始序: %s", scts_time);
    pos += 7;  
    
    // 7. UDL 数据区总长解析
    uint8_t udl = bin[pos++];  
    uint8_t *ud_payload = &bin[pos]; 
    size_t ud_len = is_7bit ? ((udl * 7 + 7) / 8) : udl;
    WriteLog("[PDU剖析] [7/9 字段] 用户数据长度(UDL): 0x%02X (计数单位: %s 为 %d，换算二进制空间: %zu 字节)", 
             udl, is_7bit ? "字符" : "字节", udl, ud_len);

    if (pos + ud_len > bin_len) {
        WriteLog("[PDU剖析] [边界错误] 声明的用户数据空间超出PDU真实物理边界");
        free(bin); return false;
    }
    
    // 8. UDH 链式长短信头深度拆解
    size_t udh_total_len = 0;
    if (has_udh && ud_len > 0) {  
        uint8_t udhl = ud_payload[0];
        udh_total_len = 1 + udhl;
        WriteLog("[PDU剖析] [8/9 字段] 检测到用户数据头(UDH)！UDHL长度指示: %d 字节, UDH总占位: %zu 字节", udhl, udh_total_len);
        
        if (udhl > 0 && udhl + 1 <= ud_len) {
            size_t ie_pos = 1;
            while (ie_pos + 2 <= (size_t)udhl + 1) {
                uint8_t iei = ud_payload[ie_pos]; 
                uint8_t iedl = ud_payload[ie_pos + 1];
                WriteLog("[PDU剖析] └── UDH 信息元素节点 -> IEI(标识): 0x%02X, IEDL(数据长度): %d", iei, iedl);
                
                if (ie_pos + 2 + iedl > (size_t)udhl + 1) break;
                
                if (iei == 0x00 && iedl == 0x03) { 
                    frame->is_concat = true; 
                    frame->is_16bit_ref = false;
                    frame->ref_num = ud_payload[ie_pos + 2]; 
                    frame->total_parts = ud_payload[ie_pos + 3]; 
                    frame->part_num = ud_payload[ie_pos + 4];
                    WriteLog("[PDU剖析]     └── [长短信匹配成功] 8位Ref标识: %d, 总片数: %d, 当前片序: %d", 
                             frame->ref_num, frame->total_parts, frame->part_num);
                    break;
                } else if (iei == 0x08 && iedl == 0x04) { 
                    frame->is_concat = true; 
                    frame->is_16bit_ref = true;
                    frame->ref_num = (ud_payload[ie_pos + 2] << 8) | ud_payload[ie_pos + 3]; 
                    frame->total_parts = ud_payload[ie_pos + 4]; 
                    frame->part_num = ud_payload[ie_pos + 5];
                    WriteLog("[PDU剖析]     └── [长短信匹配成功] 16位大Ref标识: %d, 总片数: %d, 当前片序: %d", 
                             frame->ref_num, frame->total_parts, frame->part_num);
                    break;
                }
                ie_pos += 2 + iedl;
            }
            
            // 重新校准用户纯正文长度
            if (is_7bit) {
                size_t udh_bits = udh_total_len * 8;
                size_t shift_bits = (7 - (udh_bits % 7)) % 7;
                frame->data_len = udl - ((udh_bits + shift_bits) / 7);
            } else {
                frame->data_len = ud_len - udh_total_len;
            }
        } else {
            WriteLog("[PDU剖析] [格式错误] UDH声明区长度异常");
            free(bin); return false;
        }
    } else {  
        frame->is_concat = false; 
        frame->data_len = is_7bit ? udl : ud_len;   
    }
    
    // 9. 纯正文 Payload 区域截取
    WriteLog("[PDU剖析] [9/9 字段] 剥离全部协议头后，锁定的纯正文(UserData)物理净荷跨度: %zu 字节", frame->data_len);
    if (frame->data_len > sizeof(frame->msg_data) - 1) 
        frame->data_len = sizeof(frame->msg_data) - 1;
    
    if (udh_total_len < ud_len) {
        size_t copy_bytes = is_7bit ? (ud_len - udh_total_len) : frame->data_len;
        if (copy_bytes > sizeof(frame->msg_data) - 1)
            copy_bytes = sizeof(frame->msg_data) - 1;
        memcpy(frame->msg_data, ud_payload + udh_total_len, copy_bytes);
        frame->data_len = copy_bytes;
        frame->msg_data[frame->data_len] = '\0';
    } else {
        frame->data_len = 0;
        frame->msg_data[0] = '\0';
    }
    
    // 打印原始未还原的正文16进制内容
    char raw_ud_hex[256] = {0};
    for(size_t x = 0; x < (frame->data_len > 32 ? 32 : frame->data_len); x++) {
        sprintf(raw_ud_hex + strlen(raw_ud_hex), "%02X ", frame->msg_data[x]);
    }
    WriteLog("[PDU剖析] 用户未解码正文前32字节流快照: [ %s%s]", raw_ud_hex, frame->data_len > 32 ? "..." : "");
    WriteLog("[PDU剖析] >>>>>>>>>> 核心字段切片完毕，交付上层解码组装区 <<<<<<<<<<\n");
    
    free(bin);
    return (!frame->is_concat || (frame->total_parts > 0 && frame->part_num <= frame->total_parts));
}

void CloseCurrentSerial() { 
    if (hSerial != INVALID_HANDLE_VALUE) { 
        WriteLog("[串口通信] 正在安全释放并且关闭当前独占的硬件串行端口...");
        CloseHandle(hSerial); 
        hSerial = INVALID_HANDLE_VALUE; 
    } 
}

HANDLE OpenSingleCom(LPCWSTR portName, DWORD baud) {
    WriteLog("[硬件链路] 正在尝试建立与终端串口 %ws 的独占式通信握手...", portName);
    HANDLE h = CreateFileW(portName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    
    SetupComm(h, 1024 * 1024, 1024 * 1024); 

    DCB dcb = {0}; 
    dcb.DCBlength = sizeof(DCB); 
    GetCommState(h, &dcb);
    dcb.BaudRate = baud; 
    dcb.ByteSize = 8; 
    dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    SetCommState(h, &dcb);
    COMMTIMEOUTS timeouts = {0}; 
    timeouts.ReadIntervalTimeout = 20; 
    timeouts.ReadTotalTimeoutConstant = AT_TEST_TIMEOUT;
    SetCommTimeouts(h, &timeouts); 
    return h;
}

BOOL TestComHasModule(HANDLE hCom) {
    if (hCom == INVALID_HANDLE_VALUE) return FALSE;
    PurgeComm(hCom, PURGE_RXCLEAR | PURGE_TXCLEAR);
    
    if (!SerialWriteAll(hCom, "AT\r\n", 4))
        return FALSE;

    char recvBuf[256] = {0}; 
    size_t recvLen = 0;
    DWORD tickStart = GetTickCount(), readLen = 0;
    while (GetTickCount() - tickStart < 1000) {
        char temp[64] = {0};
        if (ReadFile(hCom, temp, sizeof(temp)-1, &readLen, NULL) && readLen > 0) {
            size_t copy = (size_t)readLen;
            if (recvLen + copy >= sizeof(recvBuf))
                copy = sizeof(recvBuf) - recvLen - 1;

            if (copy > 0) {
                memcpy(recvBuf + recvLen, temp, copy);
                recvLen += copy;
                recvBuf[recvLen] = '\0';
            }

            if (strstr(recvBuf, "OK") || strstr(recvBuf, "ok")) return TRUE;
        }
        Sleep(20);
    }
    return FALSE;
}

BOOL AutoDetectComPort() {
    WCHAR portName[32];
    WriteLog("[硬件探测] 启动Air780串口自动巡检，探测区间 [COM1 ~ COM%d]...", MAX_COM_NUM);
    for (int i = 1; i <= MAX_COM_NUM; i++) {
        swprintf_s(portName, sizeof(portName) / sizeof(portName[0]), L"COM%d", i);
        HANDLE hTest = OpenSingleCom(portName, BAUD_RATE);
        if (hTest == INVALID_HANDLE_VALUE) continue;
        if (TestComHasModule(hTest)) { 
            wcscpy_s(g_ValidComPort, _countof(g_ValidComPort), portName); 
            CloseHandle(hTest); 
            WriteLog("[硬件探测] 成功发现Air780模组硬件实体！锁定的目标端口为: %ws", portName);
            return TRUE; 
        }
        CloseHandle(hTest);
    }
    WriteLog("[硬件探测] 严重警告：遍历完全部常用端口，均未寻寻找有效的Air780应答模块！");
    return FALSE;
}

void free_pending_sms(PendingSMS *node) {
    if (!node) return;
    SMSPartNode *p = node->parts_head;
    while(p) { 
        SMSPartNode *n = p->next; 
        free(p->data); free(p); p = n; 
    }
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (node == pending_queue_head) pending_queue_head = node->next;
    free(node);
}

void handle_sms_frame(int raw_table_id, SMSFrame *frame) {
    if (!frame->is_concat) {
        char utf8_buf[4096];
        WriteLog("[单片短信] 判定为单体标准短消息，直接切入解码分发...");
        
        bool success = false;
        if ((frame->dcs & 0x0C) == 0x00) {
            success = gsm7bit_to_utf8(frame->msg_data, frame->data_len, utf8_buf, sizeof(utf8_buf), 0);
        } else {
            success = ucs2be_to_utf8(frame->msg_data, frame->data_len, utf8_buf, sizeof(utf8_buf));
        }

        if (success) {
            dispatch_decoded_sms(frame->sender, 0, false, utf8_buf);
            char sql[128]; 
            snprintf(sql, sizeof(sql), "UPDATE raw_sms SET status=1 WHERE id=%d", raw_table_id); 
            mysql_query(mysql, sql);
        }
        return;
    }
    
    WriteLog("[长短信合并] 针对长短信分片进行多路复用链表寻址。Ref: %d", frame->ref_num);
    PendingSMS *target = NULL;
    for(PendingSMS *c = pending_queue_head; c; c = c->next) {
        if(strcmp(c->sender, frame->sender) == 0 && c->ref_num == frame->ref_num) { target = c; break; }
    }
    if(!target) {
        WriteLog("[长短信合并] 该Ref %d 首块分片抵达，开辟新的内存缓冲槽中...", frame->ref_num);
        target = calloc(1, sizeof(PendingSMS)); 
        strcpy(target->sender, frame->sender);
        target->ref_num = frame->ref_num; 
        target->is_16bit_ref = frame->is_16bit_ref;
        target->total_parts = frame->total_parts; 
        target->first_seen = time(NULL);
        target->next = pending_queue_head; 
        if(pending_queue_head) pending_queue_head->prev = target;
        pending_queue_head = target;
    }
    
    for(SMSPartNode *p = target->parts_head; p; p = p->next) { 
        if(p->part_num == frame->part_num) {
            WriteLog("[长短信合并] 查重忽略：当前分片号 %d 早已存在内存中", frame->part_num);
            return; 
        }
    }
    
    SMSPartNode *np = malloc(sizeof(SMSPartNode)); 
    np->part_num = frame->part_num; 
    np->data_len = frame->data_len;
    np->data = malloc(frame->data_len); 
    memcpy(np->data, frame->msg_data, frame->data_len); 
    np->next = NULL;

    if(!target->parts_head || target->parts_head->part_num > frame->part_num) { 
        np->next = target->parts_head; target->parts_head = np; 
    } else {
        SMSPartNode *cur = target->parts_head;
        while(cur->next && cur->next->part_num < frame->part_num) cur = cur->next;
        np->next = cur->next; cur->next = np;
    }
    target->arrived_count++;
    WriteLog("[长短信合并] 分片切片挂载成功 (%d/%d)。", target->arrived_count, target->total_parts);
    
    char sql[128]; 
    snprintf(sql, sizeof(sql), "UPDATE raw_sms SET status=1 WHERE id=%d", raw_table_id); 
    mysql_query(mysql, sql);

    if(target->arrived_count == target->total_parts) {
        WriteLog("[长短信合并] 欢呼吧！所有关联分片已全数集结完毕，启动数据流拼装组装...");
        size_t total = 0; 
        for(SMSPartNode *p = target->parts_head; p; p = p->next) total += p->data_len;
        uint8_t *full = malloc(total); 
        size_t off = 0; 
        for(SMSPartNode *p = target->parts_head; p; p = p->next) { 
            memcpy(full + off, p->data, p->data_len); 
            off += p->data_len; 
        }
        char *utf8 = malloc(total * 3 + 1);
        
        bool success = false;
        if ((frame->dcs & 0x0C) == 0x00) {
            // 长短信合并时的 7bit 解码需要计算 UDH 留下的 shift 位移，代码框架默认以 ucs2 传输较广，此处保留一致性逻辑
            success = gsm7bit_to_utf8(full, total, utf8, total * 3 + 1, 0);
        } else {
            success = ucs2be_to_utf8(full, total, utf8, total * 3 + 1);
        }

        if(success) {
            dispatch_decoded_sms(target->sender, target->ref_num, target->is_16bit_ref, utf8);
        }
        free(full); free(utf8); 
        free_pending_sms(target);
    }
}

void run_garbage_collection() {
    time_t now = time(NULL); PendingSMS *curr = pending_queue_head;
    while(curr) { 
        PendingSMS *next = curr->next; 
        if(now - curr->first_seen > SEGMENT_TIMEOUT_SEC) { 
            WriteLog("[内存降噪] 发现超时的残缺长短信片段（Ref: %d, 号码: %s），执行强行销毁清洗", 
                     curr->ref_num, curr->sender);
            free_pending_sms(curr); 
        } 
        curr = next; 
    }
}

// ===================== 重构解析器，完美融合兼容 +CMGL 和 +CMT =====================
void ParsePduSmsResponse(char* atResp) {
    static char streamBuf[AT_BUF];
    static size_t streamLen = 0;
    static int currentMsgIndex = -1;
    static bool isLiveCmtReport = false;

    if (!atResp || atResp[0] == '\0') return;

    size_t inLen = strlen(atResp);
    if (inLen == 0) return;

    if (inLen >= sizeof(streamBuf)) {
        atResp += (inLen - (sizeof(streamBuf) - 1));
        inLen = sizeof(streamBuf) - 1;
    }

    if (streamLen + inLen >= sizeof(streamBuf)) {
        size_t drop = (streamLen + inLen) - (sizeof(streamBuf) - 1);
        if (drop >= streamLen) {
            streamLen = 0;
        } else {
            memmove(streamBuf, streamBuf + drop, streamLen - drop);
            streamLen -= drop;
        }
        streamBuf[streamLen] = '\0';
        WriteLog("[串口缓冲] 接收缓冲区接近满载，已丢弃最旧 %zu 字节以维持连续读取", drop);
    }

    memcpy(streamBuf + streamLen, atResp, inLen);
    streamLen += inLen;
    streamBuf[streamLen] = '\0';

    size_t cursor = 0;
    while (cursor < streamLen) {
        char *eol = strstr(streamBuf + cursor, "\r\n");
        if (!eol) break;

        size_t lineLen = (size_t)(eol - (streamBuf + cursor));
        if (lineLen > 0) {
            char line[2048];
            size_t copyLen = lineLen;
            if (copyLen >= sizeof(line))
                copyLen = sizeof(line) - 1;

            memcpy(line, streamBuf + cursor, copyLen);
            line[copyLen] = '\0';

            if (strstr(line, "+CIEV:") != NULL) {
                /* Ignore noisy modem indication lines unrelated to SMS payload. */
            } else if (strstr(line, "+CMGL:") != NULL) {
                sscanf_s(line, "+CMGL: %d,", &currentMsgIndex);
                isLiveCmtReport = false;
                WriteLog("[数据流流向] 判定输出为 -> [定时巡检历史缓存]，解析索引坑位: %d", currentMsgIndex);
            } else if (strstr(line, "+CMT:") != NULL) {
                currentMsgIndex = -1;
                isLiveCmtReport = true;
                WriteLog("[数据流流向] 判定输出为 -> [实时主动通知上报 (+CMT事件)]");
            } else if ((currentMsgIndex != -1 || isLiveCmtReport) && strlen(line) > 10 && !strstr(line, "OK")) {
                WriteLog("[数据落库] PDU原始串送入缓冲层: %s", line);
                char sql[AT_BUF + 256], raw_esc[AT_BUF];
                mysql_real_escape_string(mysql, raw_esc, line, strlen(line));

                int savedIndex = (currentMsgIndex != -1) ? currentMsgIndex : 0;
                sprintf(sql, "INSERT INTO raw_sms(msg_index, raw_content, status) VALUES(%d, '%s', 0)", savedIndex, raw_esc);

                if (mysql_query(mysql, sql) == 0) {
                    int last_id = (int)mysql_insert_id(mysql);
                    WriteLog("[数据落库] 原始PDU成功持久化存储至表 raw_sms (分配的自增ID: %d)", last_id);
                    SMSFrame frame;
                    if (parse_raw_pdu(line, &frame)) {
                        handle_sms_frame(last_id, &frame);
                    } else {
                        WriteLog("[PDU致命错] parse_raw_pdu 解码引擎异常崩溃，无法完成解构！");
                    }
                } else {
                    WriteLog("[数据库异常] 写入原始数据表 raw_sms 发生阻塞错误: %s", mysql_error(mysql));
                }

                if (currentMsgIndex != -1) {
                    char delCmd[64];
                    sprintf_s(delCmd, sizeof(delCmd), "AT+CMGD=%d\r\n", currentMsgIndex);
                    WriteLog("[芯片清理] 正在对SIM卡槽的存储数据进行腾空，发送指令: AT+CMGD=%d", currentMsgIndex);
                    if (!SerialWriteAll(hSerial, delCmd, (DWORD)strlen(delCmd))) {
                        WriteLog("[芯片清理] 删除短信指令下发失败，串口写入异常");
                    } else {
                        char tmp[BUF_LEN];
                        SerialCollectUntilQuiet(hSerial, tmp, sizeof(tmp), 600, 120);
                    }
                }

                currentMsgIndex = -1;
                isLiveCmtReport = false;
            }
        }

        cursor = (size_t)(eol - streamBuf) + 2;
    }

    if (cursor > 0) {
        size_t remain = streamLen - cursor;
        memmove(streamBuf, streamBuf + cursor, remain);
        streamLen = remain;
        streamBuf[streamLen] = '\0';
    }
}

// ===================== 系统初始化与数据表建立 =====================
BOOL TablesAndMysqlInit() {
    WriteLog("[初始化] 正在构建后端底层关联数据源的连通性...");
    mysql = db_connect(); 
    if (!mysql) return FALSE;
        
    mysql_query(mysql, "CREATE TABLE IF NOT EXISTS raw_sms (id INT AUTO_INCREMENT PRIMARY KEY, msg_index INT, raw_content TEXT, receive_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP, status INT) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    mysql_query(mysql, "CREATE TABLE IF NOT EXISTS decoded_sms (id INT AUTO_INCREMENT PRIMARY KEY, sender VARCHAR(32), ref_num INT, is_16bit TINYINT, message_content TEXT, target_wxid VARCHAR(128), send_status INT DEFAULT 0, receive_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    mysql_query(mysql, "ALTER TABLE decoded_sms ADD COLUMN target_wxid VARCHAR(128);");
    mysql_query(mysql, "ALTER TABLE decoded_sms ADD COLUMN send_status INT DEFAULT 0;");
    
    WriteLog("[初始化] 基础核心业务数据依赖表检查/创建完毕，连接就绪");
    return TRUE;
}

// ===================== 主循环实时双向捕获模型 =====================
void ServiceWorkLoop() {
    if (!AutoDetectComPort() || (hSerial = OpenSingleCom(g_ValidComPort, BAUD_RATE)) == INVALID_HANDLE_VALUE || !TablesAndMysqlInit()) {
        WriteLog("[服务核心] [致命] 硬件链路启动中断：由于串口被篡改冲突或数据库配置错误");
        return;
    }
    WriteLog("[服务核心] 通信总线已打通。准备对Air780配置高频主动上报机制参数...");
    
    DWORD recv;
    char buf[32] = {0};
    
    if (!SerialWriteAll(hSerial, "AT+CMGF=0\r\n", 11)) {
        WriteLog("[配置失败] AT+CMGF=0 下发失败，串口写入异常");
        return;
    }
    if (SerialCollectUntilQuiet(hSerial, buf, sizeof(buf), 1500, 150) && strstr(buf, "OK"))
        WriteLog("[配置注入] AT+CMGF=0 切换至高效PDU模式命令下发成功");
    else {
        WriteLog("[配置失败] AT+CMGF=0 响应未通过。模组初始化流发生阻断");
        return;
    }
    
    if (!SerialWriteAll(hSerial, "AT+CNMI=2,2,0,0,0\r\n", 19)) {
        WriteLog("[配置失败] AT+CNMI 下发失败，串口写入异常");
        return;
    }
    buf[0] = '\0';
    if (SerialCollectUntilQuiet(hSerial, buf, sizeof(buf), 1500, 150) && strstr(buf, "OK"))
        WriteLog("[配置注入] AT+CNMI=2,2,0,0,0 芯片URC直达串口上报指令注入成功");
    else {
        WriteLog("[配置失败] AT+CNMI 配置未获OK核准。将丢失实时通知功能");
        return;
    }

    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    WriteLog("[服务核心] ===============================================");
    WriteLog("[服务核心]   主循环就绪：系统切换进入100ms双保险高频轮询模式");
    WriteLog("[服务核心] ===============================================");

    while (1) {
        char resp[AT_BUF] = {0};
        
        if (!g_bDebugMode) {
            if (WaitForSingleObject(g_hStopEvent, 100) == WAIT_OBJECT_0) break; 
        } else {
            Sleep(100);
        }

        if (SerialCollectUntilQuiet(hSerial, resp, sizeof(resp), 300, 50)) {
            ParsePduSmsResponse(resp);
        }
        
        static DWORD lastPollTick = 0;
        if (GetTickCount() - lastPollTick >= POLL_INTERVAL) {
            memset(resp, 0, AT_BUF);
            if (!SerialWriteAll(hSerial, "AT+CMGL=4\r\n", 11)) {
                WriteLog("[双保险主动轮询] 下发 AT+CMGL=4 失败，串口写入异常");
            } else if (SerialCollectUntilQuiet(hSerial, resp, AT_BUF, 2200, 200) && strstr(resp, "+CMGL:")) {
                WriteLog("[双保险主动轮询] 发现SIM卡内存在未处理的存量死角信息，强制召回同步");
                ParsePduSmsResponse(resp);
            }
            lastPollTick = GetTickCount();
        }
        
        run_garbage_collection();
    }
    CloseCurrentSerial();
    if(mysql) mysql_close(mysql);
}

// ===================== Windows 服务基础框架 =====================
DWORD WINAPI ServiceHandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
    if (dwControl == SERVICE_CONTROL_STOP) {
        WriteLog("[系统控制] 接收到 Windows 服务控制中心发出的 [STOP] 停机指令...");
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING; 
        SetServiceStatus(g_hStatusHandle, &g_ServiceStatus);
        SetEvent(g_hStopEvent); return NO_ERROR;
    }
    SetServiceStatus(g_hStatusHandle, &g_ServiceStatus); return NO_ERROR;
}

void CoreBusinessInit() {
    init_exe_dir();
    config_set_defaults();
    
    load_config_from_exe_dir();

    if (g_cfg.log_path[0] == '.' || strchr(g_cfg.log_path, ':') == NULL) {
        char resolved_log_path[MAX_PATH];
        build_path_in_exe_dir(resolved_log_path, sizeof(resolved_log_path), LOG_FILENAME);
        snprintf(g_cfg.log_path, sizeof(g_cfg.log_path), "%s", resolved_log_path);
    }

    log_fp = fopen(g_cfg.log_path, "a+");
    curl_global_init(CURL_GLOBAL_ALL);

    if (!log_fp && g_bDebugMode) {
        printf("[WARN] failed to open log file: %s\n", g_cfg.log_path);
    }

    WriteLog("[配置] WECHAT_SEND_API_URL=%s", g_cfg.wechat_send_api_url);
    WriteLog("[配置] FALLBACK_WXID=%s", g_cfg.fallback_wxid);
    WriteLog("[配置] DB_HOST=%s DB_NAME=%s DB_USER=%s", g_cfg.db_host, g_cfg.db_name, g_cfg.db_user);
    WriteLog("[配置] LOG_PATH=%s CACHE_REFRESH_INTERVAL_SEC=%d", g_cfg.log_path, g_cfg.cache_refresh_interval_sec);

    WriteLog("[核心装载] ----------------------------------------------------");
    WriteLog("[核心装载] 初始化事件触发：正在全量拉取MySQL本地路由映射表数据...");
    cache_full_refresh();

    HANDLE hRefreshThread = CreateThread(NULL, 0, CacheRefreshThreadProc, NULL, 0, NULL);
    if(hRefreshThread) CloseHandle(hRefreshThread);
    
    pthread_t t_push;
    if (pthread_create(&t_push, NULL, WeChatAsyncPushThreadProc, NULL) == 0) {
        pthread_detach(t_push); 
    } else {
        WriteLog("[核心装载] [严重错误] 独立的 pthread 异步微信消费引擎挂载流产！");
    }

    WriteLog("[核心装载] 所有基础设施线程均安全挂载，网关框架开始平稳接管业务。");
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    g_hStatusHandle = RegisterServiceCtrlHandlerEx(argv[0], ServiceHandlerEx, NULL);
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS; g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP; SetServiceStatus(g_hStatusHandle, &g_ServiceStatus);

    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    
    CoreBusinessInit();

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING; SetServiceStatus(g_hStatusHandle, &g_ServiceStatus);
    ServiceWorkLoop();
    
    curl_global_cleanup(); if (log_fp) fclose(log_fp);
    CloseHandle(g_hStopEvent);
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED; SetServiceStatus(g_hStatusHandle, &g_ServiceStatus);
}

int main(int argc, char* argv[]) {
    if (argc >= 2) {
        char binPath[1024]; GetModuleFileNameA(NULL, binPath, sizeof(binPath));
        SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (_stricmp(argv[1], "install") == 0) {
            SC_HANDLE hS = CreateServiceA(hSCM, SERVICE_NAME_A, SERVICE_DISPLAY, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binPath, NULL, NULL, NULL, NULL, NULL);
            if (hS) { SERVICE_DESCRIPTIONA desc = { SERVICE_DESC }; ChangeServiceConfig2A(hS, SERVICE_CONFIG_DESCRIPTION, &desc); CloseServiceHandle(hS); }
            CloseServiceHandle(hSCM); printf("服务安装成功！\n"); return 0;
        } else if (_stricmp(argv[1], "uninstall") == 0) {
            SC_HANDLE hS = OpenServiceA(hSCM, SERVICE_NAME_A, SERVICE_ALL_ACCESS);
            if (hS) { SERVICE_STATUS status; ControlService(hS, SERVICE_CONTROL_STOP, &status); DeleteService(hS); CloseServiceHandle(hS); }
            CloseServiceHandle(hSCM); printf("服务卸载成功！\n"); return 0;
        } 
        else if (_stricmp(argv[1], "debug") == 0) {
            g_bDebugMode = true;
            printf("==================================================\n");
            printf("  正在进入控制台实时调试模式 (按下 Ctrl+C 退出)   \n");
            printf("==================================================\n");
            
            CoreBusinessInit();
            ServiceWorkLoop();
            
            curl_global_cleanup(); if (log_fp) fclose(log_fp);
            return 0;
        }
    }
    
    SERVICE_TABLE_ENTRY ServiceTable[] = { { SERVICE_NAME_A, ServiceMain }, { NULL, NULL } };
    StartServiceCtrlDispatcher(ServiceTable); return 0;
}
