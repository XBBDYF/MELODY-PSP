/*
 * MELODY PSP — TLS/HTTPS 客户端（后端：mbedTLS 2.28 LTS）
 * ------------------------------------------------------------------
 * 设计要点（为什么是现在这个样子）：
 *
 *  1. 只包 socket，不管 socket。connect / close / setsockopt 全部留给调用者，
 *     这样 net.c 的重连、超时、DNS 缓存逻辑一行都不用改。
 *  2. 阻塞语义。调用者（http 客户端）就是「发完请求一直读到断开」，
 *     所以 tls_read/write 内部把 MBEDTLS_ERR_SSL_WANT_READ / WANT_WRITE
 *     当成「再等等」，用 2ms 睡眠重试，直到成功 / 出错 / 超过空闲上限。
 *     net.c 的 socket 是 SO_NONBLOCK 的，recv 会立刻返回 EAGAIN，
 *     正好走这条路径，不会把线程卡死在 recv 里。
 *  3. 证书校验默认开：证书链（内置 4 个根）+ 主机名严格校验，
 *     但**不校验有效期**——PSP 的 RTC 掉电后可能停在 2000 年，
 *     真机上如果按时间校验，所有 HTTPS 都会失败。tls_set_verify(0)
 *     可以连链都不校验（调试用）。
 *  4. 熵：PSP 没有 OS 熵源。这里用 sceKernelGetSystemTimeWide（微秒级）、
 *     sceRtcGetCurrentTick（RTC tick）、栈/堆地址、线程 id、调用计数，
 *     再加一个持久化的 32 字节 seed 文件（ms0:/.../tls_seed.bin），
 *     全部喂给 mbedTLS 的熵池 + CTR-DRBG。详见 README。
 *  5. 内存：每个连接约 40 KB（mbedTLS 内部 2×16 KB 收发缓冲 + 会话/证书/
 *     密钥上下文），全部走 newlib heap（应用的 2MB 池），不占静态 BSS。
 *     句柄本身用 64 字节对齐的堆块分配。
 */

#include "tls.h"

#include <pspkernel.h>
#include <psprtc.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/debug.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

/* ------------------------------------------------------------------ */
/* 可调参数                                                            */
/* ------------------------------------------------------------------ */

/* 单次 tls_read/tls_write 的空闲上限（毫秒）。注意这不是总时长：
 * 只要一直有数据在动，就可以一直跑（下载大文件就是这样）。 */
#define TLS_DEFAULT_TIMEOUT_MS   30000

/* 握手总超时（毫秒）。真机上 RSA-4096 验签 + X25519 要几百毫秒到几秒。 */
#define TLS_HANDSHAKE_TIMEOUT_MS 25000

/* 持久化 seed 文件。测试 EBOOT 用 -DMELODY_TLS_SEED_FILE=... 覆盖。 */
#ifndef MELODY_TLS_SEED_FILE
#define MELODY_TLS_SEED_FILE "./tls_seed.bin"
#endif

#define TLS_MAGIC 0x4D4C5331u   /* "MLS1" */

/* 自己的伪错误码（负数和 mbedTLS 的 -0x0xxx 不冲突，格式化时单独处理） */
#define TLS_ERR_SEND_FAILED  (-0x4E00)
#define TLS_ERR_RECV_FAILED  (-0x4C00)

/* ------------------------------------------------------------------ */
/* 日志（可选）                                                        */
/* ------------------------------------------------------------------ */

static void (*g_log)(const char *fmt, ...);

void tls_set_log(void (*fn)(const char *fmt, ...))
{
    g_log = fn;
}

static void tlog(const char *fmt, ...);

/* mbedTLS 自己的握手日志（MBEDTLS_DEBUG_C）-> tlog，只在 trace 打开时接 */
static void tls_dbg_bridge(void *ctx, int level, const char *file, int line,
                           const char *str)
{
    (void)ctx;
    (void)file;
    (void)line;
    tlog("mbed[%d] %s", level, str);
}

static void tlog(const char *fmt, ...)
{
    char buf[192];
    va_list ap;

    if (!g_log) return;
    va_start(ap, fmt);
    {
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);

        /* vsnprintf 返回「本该写入的长度」，不夹住会越界写栈（曾经因此
         * 在 PPSSPP 上被 mbedTLS 的长日志搞成随机崩溃/校验失败） */
        if (n < 0) n = 0;
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        buf[n] = 0;
    }
    va_end(ap);
    g_log("%s", buf);
}

/* ------------------------------------------------------------------ */
/* 时间                                                                */
/* ------------------------------------------------------------------ */

/* 单调毫秒（开机起算，32 位回绕用无符号减法处理） */
static unsigned int now_ms(void)
{
    return (unsigned int)(sceKernelGetSystemTimeWide() / 1000ULL);
}

/* ------------------------------------------------------------------ */
/* 熵                                                                  */
/* ------------------------------------------------------------------ */

static unsigned char g_seed[32];
static int g_seed_ok;
static unsigned int g_entropy_calls;

static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* 把当前可采的熵摊进 out。不加密学哈希，只做扩散；数据量够就行。 */
static void entropy_fill(unsigned char *out, size_t len)
{
    uint64_t words[8];
    uint64_t tick = 0;
    uint64_t wide = (uint64_t)sceKernelGetSystemTimeWide();
    uintptr_t stack_addr = (uintptr_t)&words;
    uintptr_t heap_addr = (uintptr_t)out;
    unsigned int tid = (unsigned int)sceKernelGetThreadId();
    int i;
    size_t off = 0;

    sceRtcGetCurrentTick(&tick);
    g_entropy_calls++;

    words[0] = wide;
    words[1] = tick;
    words[2] = ((uint64_t)stack_addr << 17) ^ (uint64_t)heap_addr;
    words[3] = ((uint64_t)tid << 32) ^ (uint64_t)g_entropy_calls;
    words[4] = ((uint64_t)g_seed[0]) | ((uint64_t)g_seed[1] << 8) |
               ((uint64_t)g_seed[2] << 16) | ((uint64_t)g_seed[3] << 24) |
               ((uint64_t)g_seed[4] << 32) | ((uint64_t)g_seed[5] << 40) |
               ((uint64_t)g_seed[6] << 48) | ((uint64_t)g_seed[7] << 56);
    words[5] = ((uint64_t)sceKernelGetSystemTimeLow() << 13) ^
               ((uint64_t)g_seed[8] << 5) ^ ((uint64_t)g_seed[9] << 21);
    words[6] = mix64(words[0] ^ words[1] ^ words[3]);
    words[7] = mix64(words[2] ^ words[4] ^ words[5]);

    /* 先把 8 个 word 互相扩散两轮，再输出 */
    for (i = 0; i < 8; ++i)
        words[i] = mix64(words[i] ^ words[(i + 1) & 7]);
    for (i = 0; i < 8; ++i)
        words[i] = mix64(words[i] ^ words[(i + 5) & 7] ^ mix64(tick + (uint64_t)i));

    while (off < len) {
        uint64_t v = words[(off / 8) & 7] ^ mix64(wide + (uint64_t)off + 0x5DEECE66DULL);
        size_t n = len - off;

        if (n > 8) n = 8;
        memcpy(out + off, &v, n);
        off += n;
    }
}

static void seed_load(void)
{
    FILE *fp;

    g_seed_ok = 0;
    fp = fopen(MELODY_TLS_SEED_FILE, "rb");
    if (!fp) return;
    if (fread(g_seed, 1, sizeof(g_seed), fp) == sizeof(g_seed)) {
        g_seed_ok = 1;
        tlog("tls: seed file loaded (%s)", MELODY_TLS_SEED_FILE);
    }
    fclose(fp);
}

/* 第一次运行没有 seed 文件：生成 32 字节存盘，下次开机熵就不只依赖时间了。
 * 没有记忆棒时静默跳过（不影响功能，只是熵质量差一点）。 */
static void seed_store(void)
{
    FILE *fp;
    unsigned char tmp[32];

    if (g_seed_ok) return;
    entropy_fill(tmp, sizeof(tmp));
    fp = fopen(MELODY_TLS_SEED_FILE, "wb");
    if (!fp) {
        memcpy(g_seed, tmp, sizeof(g_seed));
        tlog("tls: no seed file (ms0 not writable), in-memory seed only");
        return;
    }
    fwrite(tmp, 1, sizeof(tmp), fp);
    fclose(fp);
    memcpy(g_seed, tmp, sizeof(g_seed));
    g_seed_ok = 1;
    tlog("tls: seed file created (%s)", MELODY_TLS_SEED_FILE);
}

/* mbedTLS 熵源回调（STRONG，阈值 32 字节） */
static int psp_entropy_source(void *data, unsigned char *out, size_t len,
                              size_t *olen)
{
    (void)data;
    if (len > 64) len = 64;
    entropy_fill(out, len);
    *olen = len;
    return 0;
}

/* 万一 mbedtls_entropy 那条路失败（比如平台熵源在 PSP 上报错），
 * 直接用它给 CTR-DRBG 播种。 */
static int psp_entropy_seed_cb(void *data, unsigned char *out, size_t len)
{
    (void)data;
    entropy_fill(out, len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

static int g_inited;
static int g_ready;
static int g_verify = 1;
static int g_trace;
static const char *g_cert_dir;   /* 调试：收到的证书链 dump 目录 */

static SceUID g_lock = -1;

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int g_drbg_ok;

static mbedtls_x509_crt g_ca;
static int g_ca_ok;      /* 根证书是否解析成功（mbedtls_x509_crt_parse 返回 0 才是成功） */
static int g_ca_certs;   /* 内置根证书数量，仅用于日志 */

static const char kTrustPem[] =
    /* digicert_global_root_g2.pem */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
    "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
    "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
    "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
    "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
    "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
    "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
    "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
    "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
    "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
    "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
    "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
    "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
    "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
    "MrY=\n"
    "-----END CERTIFICATE-----\n"

    /* digicert_global_root_ca.pem */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh\n"
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD\n"
    "QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT\n"
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG\n"
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB\n"
    "CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97\n"
    "nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt\n"
    "43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P\n"
    "T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4\n"
    "gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO\n"
    "BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR\n"
    "TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw\n"
    "DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr\n"
    "hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg\n"
    "06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF\n"
    "PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls\n"
    "YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk\n"
    "CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=\n"
    "-----END CERTIFICATE-----\n"

    /* globalsign_root_ca_r3.pem */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G\n"
    "A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp\n"
    "Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4\n"
    "MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG\n"
    "A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8\n"
    "RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsT\n"
    "gHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmm\n"
    "KPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zd\n"
    "QQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZ\n"
    "XriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCAwEAAaNCMEAw\n"
    "DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+o\n"
    "LkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZU\n"
    "RUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMp\n"
    "jjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK\n"
    "6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQX\n"
    "mcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecs\n"
    "Mx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpH\n"
    "WD9f\n"
    "-----END CERTIFICATE-----\n"

    /* globalsign_root_ca_r1.pem */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDdTCCAl2gAwIBAgILBAAAAAABFUtaw5QwDQYJKoZIhvcNAQEFBQAwVzELMAkG\n"
    "A1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24gbnYtc2ExEDAOBgNVBAsTB1Jv\n"
    "b3QgQ0ExGzAZBgNVBAMTEkdsb2JhbFNpZ24gUm9vdCBDQTAeFw05ODA5MDExMjAw\n"
    "MDBaFw0yODAxMjgxMjAwMDBaMFcxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9i\n"
    "YWxTaWduIG52LXNhMRAwDgYDVQQLEwdSb290IENBMRswGQYDVQQDExJHbG9iYWxT\n"
    "aWduIFJvb3QgQ0EwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDaDuaZ\n"
    "jc6j40+Kfvvxi4Mla+pIH/EqsLmVEQS98GPR4mdmzxzdzxtIK+6NiY6arymAZavp\n"
    "xy0Sy6scTHAHoT0KMM0VjU/43dSMUBUc71DuxC73/OlS8pF94G3VNTCOXkNz8kHp\n"
    "1Wrjsok6Vjk4bwY8iGlbKk3Fp1S4bInMm/k8yuX9ifUSPJJ4ltbcdG6TRGHRjcdG\n"
    "snUOhugZitVtbNV4FpWi6cgKOOvyJBNPc1STE4U6G7weNLWLBYy5d4ux2x8gkasJ\n"
    "U26Qzns3dLlwR5EiUWMWea6xrkEmCMgZK9FGqkjWZCrXgzT/LCrBbBlDSgeF59N8\n"
    "9iFo7+ryUp9/k5DPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8E\n"
    "BTADAQH/MB0GA1UdDgQWBBRge2YaRQ2XyolQL30EzTSo//z9SzANBgkqhkiG9w0B\n"
    "AQUFAAOCAQEA1nPnfE920I2/7LqivjTFKDK1fPxsnCwrvQmeU79rXqoRSLblCKOz\n"
    "yj1hTdNGCbM+w6DjY1Ub8rrvrTnhQ7k4o+YviiY776BQVvnGCv04zcQLcFGUl5gE\n"
    "38NflNUVyRRBnMRddWQVDf9VMOyGj/8N7yy5Y0b2qvzfvGn9LhJIZJrglfCm7ymP\n"
    "AbEVtQwdpf5pLGkkeB6zpxxxYu7KyJesF12KwvhHhm4qxFYxldBniYUr+WymXUad\n"
    "DKqC5JlR3XC321Y9YeRq4VzW9v493kHMB65jUr9TU/Qr6cf9tveCX4XSQRjbgbME\n"
    "HMUfpIBvFSDJ3gyICh3WZlXi/EjJKSZp4A==\n"
    "-----END CERTIFICATE-----\n"
;

/* CTR-DRBG 不是线程安全的，所有取随机数的路径都过这把锁
 * （握手时取密钥、CBC 模式的 IV 都会调用）。 */
static int locked_rng(void *ctx, unsigned char *out, size_t len)
{
    int r;

    if (g_lock >= 0) sceKernelWaitSema(g_lock, 1, NULL);
    r = g_drbg_ok ? mbedtls_ctr_drbg_random(ctx, out, len) : -1;
    if (g_lock >= 0) sceKernelSignalSema(g_lock, 1);
    return r;
}

/* ------------------------------------------------------------------ */
/* 会话句柄                                                            */
/* ------------------------------------------------------------------ */

typedef struct TlsConn {
    unsigned int magic;
    int sock;
    int verify;
    int closed;
    int hs_ms;              /* 握手耗时 */
    int hs_end_ms;          /* 握手完成时刻（now_ms） */
    int first_byte_ms;
    int timeout_ms;
    int last_err;
    int last_errno;
    uint32_t verify_flags;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
} TlsConn;

static int conn_ok(TlsConn *c)
{
    return c && c->magic == TLS_MAGIC;
}

/* 64 字节对齐分配：句柄里放着收发相关的状态，按 PSP 的惯例对齐到 cache line */
static void *aligned_alloc64(size_t n)
{
    unsigned char *raw = (unsigned char *)malloc(n + 64 + sizeof(void *));
    unsigned char *aligned;

    if (!raw) return NULL;
    aligned = (unsigned char *)(((uintptr_t)raw + 64 + sizeof(void *)) &
                                ~(uintptr_t)63);
    ((void **)aligned)[-1] = raw;
    return aligned;
}

static void aligned_free64(void *p)
{
    if (p) free(((void **)p)[-1]);
}

/* ------------------------------------------------------------------ */
/* BIO                                                                 */
/* ------------------------------------------------------------------ */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    TlsConn *c = (TlsConn *)ctx;
    int n = (int)send(c->sock, (const char *)buf, (int)len, 0);

    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    c->last_errno = errno;
    return TLS_ERR_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    TlsConn *c = (TlsConn *)ctx;
    int n = (int)recv(c->sock, (char *)buf, (int)len, 0);

    if (n > 0) return n;
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;        /* 对端关闭 */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    c->last_errno = errno;
    return TLS_ERR_RECV_FAILED;
}

/* ------------------------------------------------------------------ */
/* 证书校验                                                            */
/* ------------------------------------------------------------------ */

static int verify_cb(void *data, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    TlsConn *c = (TlsConn *)data;

    if (!c) return 0;
    if (g_trace && crt) {
        char dn[160];
        char iss[160];

        dn[0] = 0;
        iss[0] = 0;
        mbedtls_x509_dn_gets(dn, sizeof(dn), &crt->subject);
        mbedtls_x509_dn_gets(iss, sizeof(iss), &crt->issuer);
        tlog("tls: chain[%d] raw=%d ca=%d sig_md=%d/%d subj=%s iss=%s",
             depth, (int)crt->raw.len, (int)crt->ca_istrue, (int)crt->sig_md,
             (int)crt->sig_pk, dn, iss);
        tlog("tls: chain[%d] pk_type=%d pk_bits=%d sig_len=%d ver=%d dnlen=%d",
             depth, (int)mbedtls_pk_get_type(&crt->pk),
             (int)mbedtls_pk_get_bitlen(&crt->pk), (int)crt->sig.len,
             (int)crt->version, (int)crt->subject_raw.len);
        if (g_cert_dir) {
            char path[128];

            FILE *fp;

            snprintf(path, sizeof(path), "%s/peer%d.der", g_cert_dir, depth);
            fp = fopen(path, "wb");
            if (fp) {
                fwrite(crt->raw.p, 1, crt->raw.len, fp);
                fclose(fp);
                tlog("tls: dumped %s (%d bytes)", path, (int)crt->raw.len);
            } else {
                tlog("tls: cannot dump %s", path);
            }
        }
    }
    if (!c->verify) return 0;

    /* 主机名不匹配一律拒绝（这是防中间人的关键一条）。 */
    if (*flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
        c->verify_flags |= *flags;
        tlog("tls: certificate hostname mismatch (depth %d)", depth);
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }

    /* 除了「过期 / 尚未生效」，别的（未受信任、签名错误、用途不对……）都算失败 */
    if (*flags & ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED |
                             MBEDTLS_X509_BADCERT_FUTURE)) {
        c->verify_flags |= *flags;
        tlog("tls: certificate verify failed, flags=0x%08X depth=%d",
             (unsigned)*flags, depth);
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }

    /* PSP 的 RTC 可能没设过（2000 年），有效期一律忽略。 */
    if (*flags) {
        tlog("tls: ignoring validity flags 0x%08X (depth %d, PSP RTC)",
             (unsigned)*flags, depth);
        *flags = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 只保留 AEAD 套件                                                    */
/* ------------------------------------------------------------------ */
/* 目的：1) 握手/记录层不再需要随机数（CBC 的 IV 要），并发更安全；
 *       2) PSP 上少走一条代码路径；3) 这些服务器都支持 GCM/ChaCha20。 */
static const int kCiphersuites[] = {
#ifdef MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
#endif
#ifdef MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
#endif
#ifdef MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
#endif
#ifdef MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
#endif
#ifdef MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
#endif
#ifdef MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
#endif
    0
};

/* 只用 NIST P-256 / P-384：PSP 上最快，而且避开了 X25519（mbedTLS 2.28 里
 * 它是 64 位 everest 实现，在 PPSSPP 的 JIT 上会挂）。所有主流 TLS 站点都支持。 */
static const mbedtls_ecp_group_id kCurves[] = {
#if defined(MBEDTLS_ECP_DP_SECP256R1_ENABLED)
    MBEDTLS_ECP_DP_SECP256R1,
#endif
#if defined(MBEDTLS_ECP_DP_SECP384R1_ENABLED)
    MBEDTLS_ECP_DP_SECP384R1,
#endif
    MBEDTLS_ECP_DP_NONE
};

/* ------------------------------------------------------------------ */
/* 初始化 / 结束                                                       */
/* ------------------------------------------------------------------ */

void tls_init(void)
{
    int r;
    static const char pers[] = "melody-psp-tls-v2";

    if (g_inited) return;
    g_inited = 1;

    if (g_lock < 0)
        g_lock = sceKernelCreateSema("melody_tls", 0, 1, 1, NULL);

    seed_load();
    seed_store();

    mbedtls_entropy_init(&g_entropy);
    mbedtls_entropy_add_source(&g_entropy, psp_entropy_source, NULL, 32,
                               MBEDTLS_ENTROPY_SOURCE_STRONG);

    mbedtls_ctr_drbg_init(&g_drbg);
    r = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              (const unsigned char *)pers, sizeof(pers) - 1);
    if (r != 0) {
        /* 平台熵源在 PSP 上可能不可用：直接用我们自己的采样播种 */
        tlog("tls: entropy seed failed (-0x%04X), using psp source only",
             (unsigned)-r);
        r = mbedtls_ctr_drbg_seed(&g_drbg, psp_entropy_seed_cb, NULL,
                                  (const unsigned char *)pers, sizeof(pers) - 1);
    }
    if (r != 0) {
        tlog("tls: drbg seed failed (-0x%04X)", (unsigned)-r);
        g_ready = 0;
        return;
    }
    g_drbg_ok = 1;

    mbedtls_x509_crt_init(&g_ca);
    r = mbedtls_x509_crt_parse(&g_ca, (const unsigned char *)kTrustPem,
                               strlen(kTrustPem) + 1);
    g_ca_ok = (r == 0);
    g_ca_certs = 0;
    {
        const char *q = kTrustPem;

        while ((q = strstr(q, "BEGIN CERTIFICATE")) != NULL) {
            g_ca_certs++;
            q += 10;
        }
    }
    if (!g_ca_ok) {
        tlog("tls: trust anchor parse failed (%d), certs=%d", r, g_ca_certs);
    } else {
        tlog("tls: %d trust anchors parsed, mbedtls %s", g_ca_certs,
             tls_mbedtls_version());
    }

    g_ready = 1;
    tlog("tls: ready (verify=%d)", g_verify);
}

void tls_term(void)
{
    if (!g_inited) return;

    if (g_ca_certs > 0) mbedtls_x509_crt_free(&g_ca);
    g_ca_certs = 0;
    g_ca_ok = 0;
    if (g_drbg_ok) mbedtls_ctr_drbg_free(&g_drbg);
    g_drbg_ok = 0;
    mbedtls_entropy_free(&g_entropy);
    if (g_lock >= 0) {
        sceKernelDeleteSema(g_lock);
        g_lock = -1;
    }
    g_ready = 0;
    g_inited = 0;
}

int tls_available(void)
{
    return g_ready;
}

void tls_set_verify(int on)
{
    g_verify = on ? 1 : 0;
}

void tls_set_trace(int on)
{
    g_trace = on ? 1 : 0;
}

void tls_set_cert_dir(const char *dir)
{
    g_cert_dir = dir;
}

int tls_verify_enabled(void)
{
    return g_verify;
}

const char *tls_mbedtls_version(void)
{
    static char buf[32];

    if (!buf[0]) mbedtls_version_get_string_full(buf);
    return buf;
}

/* ------------------------------------------------------------------ */
/* 握手                                                                */
/* ------------------------------------------------------------------ */

static void set_err(char *err, int errlen, const char *msg)
{
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s", msg);
}

static void describe_error(int r, TlsConn *c, char *out, int outlen)
{
    if (r == TLS_ERR_RECV_FAILED) {
        snprintf(out, (size_t)outlen, "socket recv failed (errno %d)", c->last_errno);
        return;
    }
    if (r == TLS_ERR_SEND_FAILED) {
        snprintf(out, (size_t)outlen, "socket send failed (errno %d)", c->last_errno);
        return;
    }
    if (c->verify_flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) {
        snprintf(out, (size_t)outlen, "certificate hostname mismatch");
        return;
    }
    if (c->verify_flags) {
        snprintf(out, (size_t)outlen, "certificate verify failed (flags 0x%X)",
                 (unsigned)c->verify_flags);
        return;
    }
    mbedtls_strerror(r, out, (size_t)outlen);
    if (!out[0]) {
        snprintf(out, (size_t)outlen, "tls error -0x%04X", (unsigned)-r);
    } else if (r != 0) {
        int n = (int)strlen(out);

        snprintf(out + n, (size_t)(outlen - n), " (-0x%04X)", (unsigned)-r);
    }
}

void *tls_start(int sock, const char *host, char *err, int errlen)
{
    TlsConn *c;
    int r = 0;
    unsigned int t0, t1;
    char msg[160];

    if (err && errlen > 0) err[0] = 0;
    if (sock < 0 || !host || !host[0]) {
        set_err(err, errlen, "bad args");
        return NULL;
    }

    if (!g_inited) tls_init();
    if (!g_ready) {
        set_err(err, errlen, "tls init failed");
        return NULL;
    }

    c = (TlsConn *)aligned_alloc64(sizeof(*c));
    if (!c) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->magic = TLS_MAGIC;
    c->sock = sock;
    c->verify = g_verify;
    c->timeout_ms = TLS_DEFAULT_TIMEOUT_MS;
    c->hs_ms = -1;
    c->first_byte_ms = -1;

    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ssl_init(&c->ssl);

    r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) goto fail;

    mbedtls_ssl_conf_rng(&c->conf, locked_rng, &g_drbg);
    mbedtls_ssl_conf_authmode(&c->conf, c->verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                  : MBEDTLS_SSL_VERIFY_NONE);
    if (g_ca_ok) mbedtls_ssl_conf_ca_chain(&c->conf, &g_ca, NULL);
    mbedtls_ssl_conf_verify(&c->conf, verify_cb, c);
#ifdef MBEDTLS_DEBUG_C
    if (g_trace) {
        mbedtls_debug_set_threshold(3);
        mbedtls_ssl_conf_dbg(&c->conf, tls_dbg_bridge, NULL);
    }
#endif
    if (kCiphersuites[0] != 0)
        mbedtls_ssl_conf_ciphersuites(&c->conf, kCiphersuites);
    if (kCurves[0] != MBEDTLS_ECP_DP_NONE)
        mbedtls_ssl_conf_curves(&c->conf, kCurves);
    mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_1);

    r = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (r != 0) goto fail;

    r = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (r != 0) goto fail;

    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);

    tlog("tls: handshake %s (verify=%d, ca_ok=%d, ca_certs=%d)", host,
         c->verify, g_ca_ok, g_ca_certs);
    t0 = now_ms();
    for (;;) {
        /* 用 handshake_step 而不是 handshake：出问题时能一行一行看到卡在哪一步。
         * tls_set_trace(1) 才输出，正常运行时只有一条 "handshake ok"。 */
        r = mbedtls_ssl_handshake_step(&c->ssl);
        if (g_trace)
            tlog("tls: step state=%d ret=%d", (int)c->ssl.state,
                 r ? (int)r : 0);
        if (r == 0) {
            if (c->ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER) break;
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if ((int)(now_ms() - t0) > TLS_HANDSHAKE_TIMEOUT_MS) {
                r = MBEDTLS_ERR_SSL_TIMEOUT;
                break;
            }
            sceKernelDelayThread(5000);
            continue;
        }
        break;
    }
    t1 = now_ms();
    c->hs_ms = (int)(t1 - t0);
    c->hs_end_ms = (int)t1;

    if (r != 0) goto fail;

    tlog("tls: handshake ok in %dms, %s %s", c->hs_ms,
         mbedtls_ssl_get_version(&c->ssl), mbedtls_ssl_get_ciphersuite(&c->ssl));
    return c;

fail:
    describe_error(r, c, msg, sizeof(msg));
    tlog("tls: handshake failed: %s", msg);
    set_err(err, errlen, msg);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    c->magic = 0;
    aligned_free64(c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 收发                                                                */
/* ------------------------------------------------------------------ */

int tls_read(void *h, void *buf, int len)
{
    TlsConn *c = (TlsConn *)h;
    unsigned int t0;
    int r;

    if (!conn_ok(c) || !buf || len <= 0) return -1;
    if (c->closed) return 0;

    t0 = now_ms();
    for (;;) {
        r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, (size_t)len);
        if (r > 0) {
            if (c->first_byte_ms < 0 && c->hs_end_ms > 0)
                c->first_byte_ms = (int)(now_ms() - (unsigned int)c->hs_end_ms);
            return r;
        }
        if (r == 0) {                       /* close_notify */
            c->closed = 1;
            return 0;
        }
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
            c->closed = 1;
            return 0;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if ((int)(now_ms() - t0) >= c->timeout_ms) {
                c->last_err = r;
                tlog("tls: read timeout after %dms", c->timeout_ms);
                return -1;
            }
            sceKernelDelayThread(2000);
            continue;
        }
        c->last_err = r;
        tlog("tls: read error -0x%04X", (unsigned)-r);
        return -1;
    }
}

int tls_write(void *h, const void *buf, int len)
{
    TlsConn *c = (TlsConn *)h;
    unsigned int t0;
    int off = 0;

    if (!conn_ok(c) || !buf || len < 0) return -1;
    if (len == 0) return 0;

    t0 = now_ms();
    while (off < len) {
        int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + off,
                                  (size_t)(len - off));
        if (r > 0) {
            off += r;
            t0 = now_ms();
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if ((int)(now_ms() - t0) >= c->timeout_ms) {
                c->last_err = r;
                return -1;
            }
            sceKernelDelayThread(2000);
            continue;
        }
        c->last_err = r;
        tlog("tls: write error -0x%04X", (unsigned)-r);
        return -1;
    }
    return off;
}

void tls_stop(void *h)
{
    TlsConn *c = (TlsConn *)h;

    if (!conn_ok(c)) return;
    /* 不发 close_notify：调用者通常已经读到 EOF，再发只会多一次阻塞写 */
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    c->magic = 0;
    aligned_free64(c);
}

/* ------------------------------------------------------------------ */
/* 诊断扩展                                                            */
/* ------------------------------------------------------------------ */

int tls_handshake_ms(void *h)
{
    TlsConn *c = (TlsConn *)h;

    return conn_ok(c) ? c->hs_ms : -1;
}

int tls_first_byte_ms(void *h)
{
    TlsConn *c = (TlsConn *)h;

    return conn_ok(c) ? c->first_byte_ms : -1;
}

int tls_set_timeout_ms(void *h, int ms)
{
    TlsConn *c = (TlsConn *)h;
    int old;

    if (!conn_ok(c)) return -1;
    old = c->timeout_ms;
    if (ms > 0) c->timeout_ms = ms;
    return old;
}

const char *tls_cipher_name(void *h)
{
    TlsConn *c = (TlsConn *)h;

    if (!conn_ok(c)) return "";
    return mbedtls_ssl_get_ciphersuite(&c->ssl);
}

const char *tls_protocol_name(void *h)
{
    TlsConn *c = (TlsConn *)h;

    if (!conn_ok(c)) return "";
    return mbedtls_ssl_get_version(&c->ssl);
}
