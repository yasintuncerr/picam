#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# setup-dnsmasq.sh — picam.device hostname resolution
#
# Configures dnsmasq on the Raspberry Pi so that clients connected
# via USB Ethernet (192.168.7.x) can access the Pi at:
#   http://picam.device  or  https://picam.device
#
# Usage (run on Pi):
#   chmod +x scripts/setup-dnsmasq.sh
#   sudo ./scripts/setup-dnsmasq.sh
#
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

PI_IP="192.168.7.2"
DOMAIN="picam.device"

echo "═══════════════════════════════════════════════════"
echo "  PiCam DNS Setup (dnsmasq)"
echo "═══════════════════════════════════════════════════"

# Install dnsmasq if not present
if ! command -v dnsmasq &>/dev/null; then
    echo "▸ Installing dnsmasq..."
    apt-get update -qq && apt-get install -y -qq dnsmasq
fi

# Stop systemd-resolved if it conflicts (port 53)
if systemctl is-active --quiet systemd-resolved 2>/dev/null; then
    echo "▸ Disabling systemd-resolved (conflicts with dnsmasq)..."
    systemctl stop systemd-resolved
    systemctl disable systemd-resolved
fi

# Configure dnsmasq
DNSMASQ_CONF="/etc/dnsmasq.d/picam.conf"
echo "▸ Writing dnsmasq config to ${DNSMASQ_CONF}..."

cat > "$DNSMASQ_CONF" <<EOF
# PiCam DNS — resolve picam.device to the Pi
# Only listen on the USB Ethernet interface

interface=usb0
bind-interfaces

# Static hostname resolution
address=/${DOMAIN}/${PI_IP}

# DHCP (optional — if you want the Pi to assign IPs to USB clients)
dhcp-range=192.168.7.100,192.168.7.200,12h
dhcp-option=option:dns-server,${PI_IP}

# Don't read /etc/resolv.conf for upstream DNS
no-resolv
# Use Google DNS as upstream
server=8.8.8.8
server=8.8.4.4
EOF

# Restart dnsmasq
echo "▸ Restarting dnsmasq..."
systemctl enable dnsmasq
systemctl restart dnsmasq

echo ""
echo "═══════════════════════════════════════════════════"
echo "  ✅ DNS setup complete!"
echo "═══════════════════════════════════════════════════"
echo ""
echo "  ${DOMAIN} → ${PI_IP}"
echo ""
echo "  From a connected Mac, you can now access:"
echo "    https://${DOMAIN}/"
echo ""
echo "  Note: On macOS, you may need to flush DNS cache:"
echo "    sudo dscacheutil -flushcache"
echo ""
