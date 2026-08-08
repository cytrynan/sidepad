"""Install an ipa over a fragile usbip link.

Two countermeasures against usbip bulk-transfer death (2026-08-09):
- Small write chunks (4KB default): 64KB writes die from the first packet.
- Session-split upload (256KB per AFC session, append mode): a single AFC
  session dies after ~1MB of sustained writes (the op *after* the burst —
  typically fclose — never gets a reply). Closing and reopening the AFC
  service connection every 256KB avoids the burst death entirely.

If the install RPC hangs with no progress responses at all, suspect a stuck
installd on the device (kernel log: "memorystatus_perform_idle_demotion()
timed out stuck process [installd]"). Remedy: reboot the device
(pymobiledevice3 diagnostics restart) and unlock it once, then retry.

Usage: chunked_install.py <ipa_path> [chunk_size] [per_session_bytes]
"""
import os
import sys

os.environ["USBMUXD_SOCKET_ADDRESS"] = "/var/run/usbmuxd"

from pymobiledevice3.lockdown import create_using_usbmux
from pymobiledevice3.services.afc import AfcService
from pymobiledevice3.services.installation_proxy import InstallationProxyService

ipa_path = sys.argv[1]
chunk = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
per_session = int(sys.argv[3]) if len(sys.argv) > 3 else 256 * 1024
remote = "/PublicStaging/Sidepad.ipa"
size = os.path.getsize(ipa_path)

lockdown = create_using_usbmux(usbmux_address="/var/run/usbmuxd")
print("lockdown OK:", lockdown.short_info.get("DeviceName"), flush=True)

# 前回の残骸を消してから積む（append 継ぎ足し事故の防止）
afc0 = AfcService(lockdown)
try:
    afc0.rm(remote)
except Exception:
    pass
try:
    afc0.makedirs("/PublicStaging")
except Exception:
    pass
afc0.service.close()

sent = 0
session_no = 0
with open(ipa_path, "rb") as f:
    while sent < size:
        session_no += 1
        afc = AfcService(lockdown)
        mode = "w" if sent == 0 else "a"
        h = afc.fopen(remote, mode)
        session_sent = 0
        while session_sent < per_session and sent < size:
            data = f.read(min(chunk, per_session - session_sent))
            if not data:
                break
            afc.fwrite(h, data)
            sent += len(data)
            session_sent += len(data)
        afc.fclose(h)
        afc.service.close()
        print(f"session {session_no}: total {sent}/{size}", flush=True)

# verify staged size
afc = AfcService(lockdown)
info = afc.stat(remote)
print("staged st_size:", info.get("st_size"), flush=True)
assert int(info.get("st_size")) == size, "staged size mismatch"
afc.service.close()

ip = InstallationProxyService(lockdown)
ip.service.send_plist({
    "Command": "Install",
    "PackagePath": "PublicStaging/Sidepad.ipa",
    "ClientOptions": {},
})
while True:
    resp = ip.service.recv_plist()
    if resp is None:
        print("connection closed unexpectedly")
        sys.exit(1)
    status = resp.get("Status")
    if "PercentComplete" in resp:
        print(f"install {resp['PercentComplete']}% - {status}", flush=True)
    else:
        print("resp:", resp, flush=True)
    if status == "Complete":
        print("INSTALL_COMPLETE")
        break
    if "Error" in resp:
        print("INSTALL_ERROR:", resp.get("Error"), resp.get("ErrorDescription"))
        sys.exit(1)
