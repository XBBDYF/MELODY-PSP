"""Startup connect must not burn 30s on a wrong profile and a stuck resolver.

Field log (H: owner's card):
  [ 5953] apctl connect(2) = 0x80110601 ... [7086] retry  -> profile loop only 1..3,
                                                            the saved profile was #4
  [16044] dns lookup state=4 ... [27260] dns done ms=11216   first lookup blocked 11.2s
  (another session) dns lookup at 21177 and no "dns done" for 46s until power off

So: boot must try every configured profile (last successful first), and it must wait
for the AP to advertise a DNS server before asking the SDK resolver.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
api = (ROOT / "src/api.c").read_text(encoding="utf-8")
net = (ROOT / "src/net.c").read_text(encoding="utf-8")
net_h = (ROOT / "src/net.h").read_text(encoding="utf-8")

probe = api[api.index("static void job_probe(void)"):]
probe = probe[:probe.index("static void job_connect(")]
assert "net_list_profiles(" in probe, "boot probe still guesses profile numbers"
assert "net_last_profile()" in probe, "boot probe does not prefer the last used profile"
assert "p <= 3" not in probe and "p=1" not in probe, "boot probe still limits itself to profiles 1..3"

assert "int net_last_profile(void)" in net and "int net_last_profile(void);" in net_h

# The resolver is only called once the AP reports a DNS server, and only on the
# bounded worker (the caller can walk away after DNS_JOB_DEADLINE_MS).
assert "PSP_NET_APCTL_INFO_PRIMDNS" in net and "PSP_NET_APCTL_INFO_SECDNS" in net
assert "static int apctl_dns_ready(void)" in net
assert "static int dns_wait_ready(int deadline_ms)" in net
raw = net[net.index("static int dns_lookup_raw("):]
raw = raw[:raw.index("/* from_cache: 1")]
assert "dns_wait_ready(" in raw, "lookup does not wait for a usable DNS server"
assert "dns_resolve_bounded(" in raw, "lookup still calls the SDK resolver inline"
assert "sceNetResolverStartNtoA(" not in raw, "the unbounded SDK call must live on the worker"
worker = net[net.index("static int dns_worker_step(void)"):]
worker = worker[:worker.index("static int dns_worker_main(")]
assert "sceNetResolverStartNtoA(" in worker
assert "DNS_JOB_DEADLINE_MS" in net and "dns_wait_job(" in net
assert "dns_fallback_ip(host, ip)" in raw, "a failed/timed-out lookup has no fallback path"

# Association waits for DNS too, so the first request does not pay for it.
conn = net[net.index("int net_apctl_connect(int profile, int seconds)"):]
conn = conn[:conn.index("int net_apctl_state(void);")]
assert "dns_wait_ready(6000)" in conn
print("PASS startup connect: all profiles tried (last first), DNS readiness waited before resolving")
