#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
# setup-ssl.sh — PiCam HTTPS certificate generator
#
# Creates a custom CA (Certificate Authority) and signs a server
# certificate for 192.168.7.2 with proper SAN entries.
#
# Usage (run on Pi):
#   chmod +x setup-ssl.sh
#   sudo ./setup-ssl.sh
#
# Output:
#   /etc/nginx/ssl/picam-ca.crt        ← CA cert (copy to Mac)
#   /etc/nginx/ssl/picam-server.crt    ← Nginx server certificate
#   /etc/nginx/ssl/picam-server.key    ← Nginx server private key
#
# Trust the CA on Mac:
#   scp picam@192.168.7.2:/etc/nginx/ssl/picam-ca.crt ~/Desktop/
#   sudo security add-trusted-cert -d -r trustRoot \
#        -k /Library/Keychains/System.keychain ~/Desktop/picam-ca.crt
#
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

SSL_DIR="/etc/nginx/ssl"
DAYS_CA=3650        # CA validity: 10 years
DAYS_SERVER=825     # Server validity: ~2 years (Apple limit)
IP_ADDR="192.168.7.2"
HOSTNAME="picam"

echo "═══════════════════════════════════════════════════"
echo "  PiCam SSL Certificate Generator"
echo "═══════════════════════════════════════════════════"

# Create directory
mkdir -p "$SSL_DIR"
cd "$SSL_DIR"

# ── 1. Generate CA (Certificate Authority) ──
echo ""
echo "▸ Generating CA private key..."
openssl genrsa -out picam-ca.key 4096

echo "▸ Generating CA certificate..."
openssl req -new -x509 -days "$DAYS_CA" \
    -key picam-ca.key \
    -out picam-ca.crt \
    -subj "/C=TR/ST=Istanbul/O=PiCam/CN=PiCam CA"

# ── 2. Generate server private key ──
echo "▸ Generating server private key..."
openssl genrsa -out picam-server.key 2048

# ── 3. Generate server CSR (Certificate Signing Request) ──
echo "▸ Generating server CSR..."
openssl req -new \
    -key picam-server.key \
    -out picam-server.csr \
    -subj "/C=TR/ST=Istanbul/O=PiCam/CN=${HOSTNAME}"

# ── 4. Create SAN (Subject Alternative Name) extensions file ──
#    Browsers reject certificates without SAN
cat > picam-server-ext.cnf <<EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment
subjectAltName = @alt_names

[alt_names]
IP.1 = ${IP_ADDR}
DNS.1 = ${HOSTNAME}
DNS.2 = ${HOSTNAME}.local
DNS.3 = picam.device
EOF

# ── 5. Sign server certificate with CA ──
echo "▸ Signing server certificate with CA..."
openssl x509 -req -days "$DAYS_SERVER" \
    -in picam-server.csr \
    -CA picam-ca.crt \
    -CAkey picam-ca.key \
    -CAcreateserial \
    -out picam-server.crt \
    -extfile picam-server-ext.cnf

# ── 6. Cleanup temporary files ──
rm -f picam-server.csr picam-server-ext.cnf picam-ca.srl

# ── 7. Set permissions ──
chmod 600 picam-ca.key picam-server.key
chmod 644 picam-ca.crt picam-server.crt
chown root:root "$SSL_DIR"/*

echo ""
echo "═══════════════════════════════════════════════════"
echo "  ✅ Certificates created successfully!"
echo "═══════════════════════════════════════════════════"
echo ""
echo "  Files:"
echo "    CA cert  : ${SSL_DIR}/picam-ca.crt"
echo "    Server   : ${SSL_DIR}/picam-server.crt"
echo "    Key      : ${SSL_DIR}/picam-server.key"
echo ""
echo "  ─── Next steps ───"
echo ""
echo "  1. Restart nginx:"
echo "     sudo nginx -t && sudo systemctl restart nginx"
echo ""
echo "  2. Copy CA cert to Mac:"
echo "     scp picam@${IP_ADDR}:${SSL_DIR}/picam-ca.crt ~/Desktop/"
echo ""
echo "  3. Trust it on Mac:"
echo "     sudo security add-trusted-cert -d -r trustRoot \\"
echo "          -k /Library/Keychains/System.keychain ~/Desktop/picam-ca.crt"
echo ""
echo "  4. Open in browser: https://${IP_ADDR}/"
echo ""
