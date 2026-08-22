#!/bin/sh

set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
config_file="$repo_dir/HERMES.CFG"
ca_file="$repo_dir/HERMES_CA.PEM"

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /Volumes/CARDPUTER_SD" >&2
    exit 2
fi

sd_root=$1
if [ ! -d "$sd_root" ]; then
    echo "SD mount does not exist: $sd_root" >&2
    exit 1
fi
if [ ! -f "$config_file" ]; then
    echo "Missing local config: $config_file" >&2
    exit 1
fi
if [ ! -f "$ca_file" ]; then
    echo "Missing CA certificate: $ca_file" >&2
    exit 1
fi

case "$sd_root" in
    /|/Volumes|/Volumes/ExternalSSD|/Volumes/ExternalSSD/*/github|/Volumes/ExternalSSD/*/github/*)
        echo "Refusing a broad or repository path; pass the mounted SD volume root." >&2
        exit 1
        ;;
esac

if awk '!/^[[:space:]]*[#;]/' "$config_file" |
   grep -Eq 'YOUR_WIFI|YOUR_PASSWORD|REPLACE_ME|replace-with-a-long-random-(token|password)'; then
    echo "Local HERMES.CFG still contains a placeholder." >&2
    exit 1
fi
if ! grep -Eq '^hermes_base_url=https://[^[:space:]]+' "$config_file"; then
    echo "HERMES.CFG must contain an HTTPS hermes_base_url." >&2
    exit 1
fi
if grep -Eq '^web_admin=true' "$config_file" &&
   ! grep -Eq '^web_admin_username=[^[:space:]]+' "$config_file"; then
    echo "web_admin=true requires web_admin_username." >&2
    exit 1
fi
has_login_username=false
has_login_password=false
grep -Eq '^hermes_login_username=[^[:space:]]+' "$config_file" && has_login_username=true
grep -Eq '^hermes_login_password=[^[:space:]]+' "$config_file" && has_login_password=true
if [ "$has_login_username" != "$has_login_password" ]; then
    echo "HERMES.CFG password mode requires both Hermes login fields." >&2
    exit 1
fi

auth_modes=0
grep -Eq '^hermes_session_cookie="[^"]+=[^"]+"' "$config_file" && auth_modes=$((auth_modes + 1))
grep -Eq '^hermes_session_token=[^[:space:]]+' "$config_file" && auth_modes=$((auth_modes + 1))
[ "$has_login_username" = true ] && auth_modes=$((auth_modes + 1))
if [ "$auth_modes" -ne 1 ]; then
    echo "HERMES.CFG must contain exactly one usable Hermes auth mode." >&2
    exit 1
fi
if ! grep -q 'BEGIN CERTIFICATE' "$ca_file"; then
    echo "HERMES_CA.PEM does not look like a PEM certificate." >&2
    exit 1
fi

copy_atomic() {
    source_file=$1
    target_file=$2
    temp_file="$target_file.tmp"
    rm -f "$temp_file"
    cp "$source_file" "$temp_file"
    mv -f "$temp_file" "$target_file"
}

copy_atomic "$config_file" "$sd_root/HERMES.CFG"
copy_atomic "$ca_file" "$sd_root/HERMES_CA.PEM"

echo "Copied HERMES.CFG and HERMES_CA.PEM to $sd_root"
echo "Secrets were not printed. Disable web_admin after Cookie provisioning."
