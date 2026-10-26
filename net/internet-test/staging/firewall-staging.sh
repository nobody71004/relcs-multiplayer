#!/usr/bin/env bash
# reLCS staging firewall (iptables + iptables-persistent) — idempotent.
# Run as root on the staging VPS. The dry-run does NOT execute this (firewall
# changes are the one privileged step).
#
# Each rule is added to the live INPUT chain only when missing, and saved to
# RULES_FILE only when the file lacks it, so the rules load again after a
# reboot (netfilter-persistent reads RULES_FILE at boot). Re-running changes
# nothing once the rules are in place.
#
# 22/tcp    SSH
# 7777/udp  game + in-band rcon
# 7800/udp  master list queries + server announce
# 7778/udp  LAN discovery — deliberately NOT opened (lan_discover=0 in server.cfg)
set -euo pipefail

RULES_FILE="${RULES_FILE:-/etc/iptables/rules.v4}"
RULES=(
	"-A INPUT -p tcp -m tcp --dport 22 -j ACCEPT"
	"-A INPUT -p udp -m udp --dport 7777 -j ACCEPT"
	"-A INPUT -p udp -m udp --dport 7800 -j ACCEPT"
)

if [ "$(id -u)" -ne 0 ]; then
	echo "firewall-staging: must run as root" >&2
	exit 1
fi

# 1. live chain: append each rule only when iptables does not already have it
for rule in "${RULES[@]}"; do
	spec="${rule#-A INPUT }"
	# shellcheck disable=SC2086 # spec is a list of iptables arguments
	if iptables -C INPUT $spec 2>/dev/null; then
		echo "live: present   $spec"
	else
		# shellcheck disable=SC2086
		iptables -A INPUT $spec
		echo "live: added     $spec"
	fi
done

# 2. saved rules: insert missing rules before the first INPUT rule, after
#    validating the result with iptables-restore so a bad file never replaces
#    the one the boot loader reads
missing=()
for rule in "${RULES[@]}"; do
	if ! grep -qxF -- "$rule" "$RULES_FILE"; then
		missing+=("$rule")
	fi
done

if [ "${#missing[@]}" -eq 0 ]; then
	echo "saved: $RULES_FILE already has all rules"
else
	tmp="$(mktemp)"
	backup="$RULES_FILE.bak-$(date +%Y%m%d%H%M%S)"
	printf '%s\n' "${missing[@]}" > "$tmp.new"
	awk -v ins="$tmp.new" '
		!done && /^-A INPUT / {
			while ((getline line < ins) > 0) print line
			close(ins)
			done = 1
		}
		{ print }
		END { if (!done) exit 3 }
	' "$RULES_FILE" > "$tmp" || { echo "saved: no INPUT rule found in $RULES_FILE" >&2; rm -f "$tmp" "$tmp.new"; exit 1; }
	if ! iptables-restore --test "$tmp"; then
		echo "saved: generated file failed iptables-restore --test; $RULES_FILE unchanged" >&2
		rm -f "$tmp" "$tmp.new"
		exit 1
	fi
	cp -p "$RULES_FILE" "$backup"
	install -m 640 "$tmp" "$RULES_FILE"
	rm -f "$tmp" "$tmp.new"
	for rule in "${missing[@]}"; do echo "saved: added    $rule"; done
	echo "saved: previous file kept at $backup"
fi

echo "--- live INPUT (reLCS ports):"
iptables -S INPUT | grep -E -- "--dport (22|7777|7800) " || true
echo "NOTE: open the same ports in the provider's security group if one exists."
