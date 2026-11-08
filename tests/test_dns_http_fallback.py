"""Plain-HTTP DNS fallback: when the carrier's DNS cannot resolve the CDN hosts.

Field log (14:47): every lookup for m7/m701/m801.music.126.net hit the 2.5s deadline
(`dns done ms=2502 result=-1`) while cached songs played, so uncached songs failed
with "网络连接失败". The fallback asks DNSPod by IP over plain HTTP, which needs no
name resolution itself, and the answered address is cached like any other.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
net = (ROOT / "src/net.c").read_text(encoding="utf-8")
body = net[net.index("static volatile int g_dns_http_busy;"):
           net.index("static int dns_lookup(")]

PRE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct in_addr { unsigned int s_addr; };
static const char *fixture;
static unsigned ip_pack(const unsigned char *b)
{
    return (unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
}
/* Minimal dotted-quad parser, same contract as inet_aton for these fixtures. */
static int inet_aton(const char *s, struct in_addr *a)
{
    unsigned parts[4] = {0, 0, 0, 0};
    int n = 0;
    const char *p = s;
    for (; *p && n < 4; ++p) {
        if (*p == '.') { ++n; continue; }
        if (*p < '0' || *p > '9') return 0;
        parts[n] = parts[n] * 10u + (unsigned)(*p - '0');
        if (parts[n] > 255u) return 0;
    }
    if (n != 3) return 0;
    unsigned char b[4] = {(unsigned char)parts[0], (unsigned char)parts[1],
                          (unsigned char)parts[2], (unsigned char)parts[3]};
    memcpy(&a->s_addr, b, 4);
    return 1;
}
static int http_get(const char *url, char *out, int outmax, int *outlen, int timeout_ms,
                    char *err, int errlen)
{
    int n;
    (void)timeout_ms; (void)err; (void)errlen; (void)url;
    if (!fixture) return -1;
    n = (int)strlen(fixture);
    if (n >= outmax) n = outmax - 1;
    memcpy(out, fixture, (size_t)n);
    out[n] = 0;
    *outlen = n;
    return 0;
}
'''
MAIN = r'''
int main(void)
{
    unsigned ip = 0;

    fixture = "112.28.172.53,60";
    assert(dns_http_query("m7.music.126.net", &ip) == 1);
    assert(ip == ((112u << 24) | (28u << 16) | (172u << 8) | 53u));

    fixture = "112.28.172.53";
    assert(dns_http_query("m701.music.126.net", &ip) == 1);

    fixture = "1.2.3.4;5.6.7.8";
    assert(dns_http_query("x.example", &ip) == 1 && ip == 0x01020304u);

    fixture = "0";
    assert(dns_http_query("x.example", &ip) == 0);
    fixture = "not-an-ip";
    assert(dns_http_query("x.example", &ip) == 0);
    fixture = "";
    assert(dns_http_query("x.example", &ip) == 0);
    fixture = NULL;
    assert(dns_http_query("x.example", &ip) == 0);

    /* Recursion guard: the request for the resolver IP must not re-enter. */
    fixture = "112.28.172.53";
    g_dns_http_busy = 1;
    assert(dns_http_query("m7.music.126.net", &ip) == 0);
    g_dns_http_busy = 0;
    assert(dns_http_query("m7.music.126.net", &ip) == 1);

    puts("PASS HTTP DNS fallback: ip[/ttl|;ip], rejects junk, recursion guarded");
    return 0;
}
'''
out_dir = ROOT / ".tmp" / "dns-http-test"
out_dir.mkdir(parents=True, exist_ok=True)
c_file = out_dir / "test.c"
c_file.write_text(PRE + body + MAIN, encoding="utf-8")
exe = out_dir / "test.exe"
subprocess.run(["D:/msys64/ucrt64/bin/gcc.exe", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", str(c_file), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)

lookup = net[net.index("static int dns_lookup(const char *host"):]
lookup = lookup[:lookup.index("int net_manual_connect(")]
assert "dns_http_query(host, ip)" in lookup, "lookup never falls back to HTTP DNS"
assert lookup.index("dns_http_query(host, ip)") < lookup.index("dns_fallback_ip(host, ip)"), \
    "HTTP DNS must be tried before the hard-coded addresses"
print("PASS dns_lookup order: SDK -> HTTP DNS -> built-in address")
