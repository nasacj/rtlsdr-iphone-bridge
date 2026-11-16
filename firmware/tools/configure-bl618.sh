#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Says what the next BL618 build is for: which network the board uses and the name it
# answers to. With two boards on one phone — one receiver each — every board needs its own
# name, and this is where it gets one.
#
#   tools/configure-bl618.sh --join "My iPhone" --name rtlsdr-atc
#       The board joins that network (a phone's Personal Hotspot) and answers to
#       rtlsdr-atc.local. The password is asked for, or taken from $BRIDGE_STA_KEY.
#   tools/configure-bl618.sh --own --name rtlsdr
#       The board opens its own network, as it does with no settings at all.
#   tools/configure-bl618.sh --show          what the next build will be
#   tools/configure-bl618.sh --clear         back to wifi_secrets.h and the defaults
#
# Then build and flash as usual (tools/build-all.sh, tools/flash-bl618.sh). The settings
# are compiled in, so each board is: configure, build, flash.
#
# It writes bl618_rtl_udp/board_settings.h, which is not committed and overrides
# wifi_secrets.h for what it sets. A board told to join a network still falls back to its
# own after 20 seconds, so a wrong password does not lock anybody out.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/bl618_rtl_udp/board_settings.h"

usage() { sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
show() {
  if [ ! -f "$out" ]; then echo "no board settings: wifi_secrets.h and the defaults apply"; return; fi
  # The one line that says it, kept as a comment at the top of the file; never the password.
  sed -n 's|^// summary: ||p' "$out"
}
# A C string literal's two special characters. UTF-8 goes through as it is, so a curly
# apostrophe in a hotspot's name is kept.
cstring() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

mode="" ssid="" name=""
while [ $# -gt 0 ]; do
  case "$1" in
    --join) mode=join; ssid=${2:?--join needs the network name}; shift 2 ;;
    --own) mode=own; shift ;;
    --name) name=${2:?--name needs a name}; shift 2 ;;
    --show) show; exit 0 ;;
    --clear) rm -f "$out"; show; exit 0 ;;
    *) usage ;;
  esac
done
[ -n "$mode" ] || usage
name=${name:-rtlsdr}
# One mDNS label: what a host name may hold, and short enough for "-2" if it is taken.
if ! printf '%s' "$name" | grep -Eq '^[a-z0-9]([a-z0-9-]{0,58}[a-z0-9])?$'; then
  echo "name \"$name\": lower-case letters, digits and hyphens only (rtlsdr-atc, not rtlsdr.atc)"; exit 1
fi

# printf, not echo: some shells' echo rewrites the backslashes cstring has just doubled.
{
  if [ "$mode" = join ]; then
    key=${BRIDGE_STA_KEY:-}
    if [ -z "$key" ]; then
      printf 'Password for "%s": ' "$ssid" >&2
      stty -echo 2>/dev/null || true
      IFS= read -r key
      stty echo 2>/dev/null || true
      printf '\n' >&2
    fi
    n=$(printf '%s' "$key" | wc -c | tr -d ' ')
    if [ "$n" -lt 8 ] || [ "$n" -gt 63 ]; then echo "a WPA2 password is 8 to 63 characters" >&2; exit 1; fi
    printf '%s\n' "// summary: joins \"$ssid\", answers to $name.local"
    printf '%s\n' "// Written by tools/configure-bl618.sh. Not committed."
    printf '%s\n' "#undef BRIDGE_STA_SSID"
    printf '%s\n' "#define BRIDGE_STA_SSID \"$(cstring "$ssid")\""
    printf '%s\n' "#undef BRIDGE_STA_KEY"
    printf '%s\n' "#define BRIDGE_STA_KEY \"$(cstring "$key")\""
  else
    printf '%s\n' "// summary: opens its own network, answers to $name.local"
    printf '%s\n' "// Written by tools/configure-bl618.sh. Not committed."
    printf '%s\n' "#undef BRIDGE_STA_SSID"
    printf '%s\n' "#define BRIDGE_STA_SSID \"\""
  fi
  printf '%s\n' "#undef BRIDGE_MDNS_NAME"
  printf '%s\n' "#define BRIDGE_MDNS_NAME \"$name\""
} > "$out.tmp"
mv "$out.tmp" "$out"
chmod 600 "$out"
show
