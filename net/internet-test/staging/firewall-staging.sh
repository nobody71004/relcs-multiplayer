#!/usr/bin/env bash
# reLCS staging firewall (ufw) — idempotent. Run as root/admin; the dry-run
# does NOT execute this (firewall changes are the one privileged step).
#
# 7777/udp  game + in-band rcon
# 7800/udp  master list queries + server announce
# 22/tcp    SSH (tighten with: ufw allow from <admin-ip> to any port 22)
# 7778/udp  LAN discovery — deliberately NOT opened (lan_discover=0 in server.cfg)
set -euo pipefail

ufw allow 22/tcp
ufw allow 7777/udp
ufw allow 7800/udp

ufw status numbered
echo "NOTE: open the same ports in the provider's security group if one exists."
