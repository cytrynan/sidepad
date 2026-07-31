#!/bin/bash
# Full xtool prerequisite install on a clean Ubuntu 24.04 WSL distro
set -x
export DEBIAN_FRONTEND=noninteractive

apt-get update -y
apt-get install -y --no-install-recommends \
  curl wget ca-certificates git unzip zip file gnupg2 \
  build-essential pkg-config binutils \
  libssl-dev libcurl4-openssl-dev libxml2-dev liblzma-dev zlib1g-dev \
  libncurses-dev libpython3-dev \
  usbmuxd libimobiledevice6 libimobiledevice-utils usbutils \
  libfuse2t64 python3
echo "=== apt exit:$? ==="

# --- Swift 6.3.3 (native ubuntu24.04 build) ---
mkdir -p /opt/swift /root/dl && cd /root/dl
curl -fL -o swift.tar.gz \
  "https://download.swift.org/swift-6.3.3-release/ubuntu2404/swift-6.3.3-RELEASE/swift-6.3.3-RELEASE-ubuntu24.04.tar.gz" || exit 1
ls -lh swift.tar.gz
tar xzf swift.tar.gz -C /opt/swift --strip-components=1
echo 'export PATH=/opt/swift/usr/bin:$PATH' > /etc/profile.d/swift.sh
chmod +x /etc/profile.d/swift.sh
export PATH=/opt/swift/usr/bin:$PATH

echo "=== missing libs check ==="
for f in /opt/swift/usr/bin/*; do
  [ -x "$f" ] || continue
  ldd "$f" 2>/dev/null | grep "not found" | sed "s|^|$(basename $f): |"
done | sort -u
echo "(empty above = all resolved)"

echo "=== swift --version ==="
swift --version
echo "=== swift sdk list ==="
swift sdk list; echo "exit=$?"

# --- xtool ---
curl -fL "https://github.com/xtool-org/xtool/releases/latest/download/xtool-$(uname -m).AppImage" \
  -o /usr/local/bin/xtool || exit 1
chmod +x /usr/local/bin/xtool
echo "=== xtool --version ==="
xtool --version
echo "=== xtool sdk status ==="
xtool sdk status 2>&1
echo "=== xtool auth status ==="
xtool auth status 2>&1
echo "=== rm swift tarball to reclaim space ==="
rm -f /root/dl/swift.tar.gz
df -h / | tail -1
