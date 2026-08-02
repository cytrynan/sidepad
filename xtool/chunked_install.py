"""Install an ipa over a fragile usbip link by uploading in tiny AFC chunks.

Usage: chunked_install.py <ipa_path> [chunk_size]
"""
import os
import sys

os.environ["USBMUXD_SOCKET_ADDRESS"] = "/var/run/usbmuxd"

from pymobiledevice3.lockdown import create_using_usbmux
from pymobiledevice3.services.afc import AfcService
from pymobiledevice3.services.installation_proxy import InstallationProxyService

ipa_path = sys.argv[1]
chunk = int(sys.argv[2]) if len(sys.argv) > 2 else 4096
remote = "/PublicStaging/Sidepad.ipa"

lockdown = create_using_usbmux(usbmux_address="/var/run/usbmuxd")
print("lockdown OK:", lockdown.short_info.get("DeviceName"))

afc = AfcService(lockdown)
try:
    afc.makedirs("/PublicStaging")
except Exception:
    pass

size = os.path.getsize(ipa_path)
h = afc.fopen(remote, "w")
sent = 0
with open(ipa_path, "rb") as f:
    while True:
        data = f.read(chunk)
        if not data:
            break
        afc.fwrite(h, data)
        sent += len(data)
        if sent % (chunk * 10) == 0 or sent == size:
            print(f"uploaded {sent}/{size}", flush=True)
afc.fclose(h)
print("upload done")

# verify staged size
info = afc.stat(remote)
print("staged st_size:", info.get("st_size"))
assert int(info.get("st_size")) == size, "staged size mismatch"

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
