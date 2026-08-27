#!/bin/bash
#
# Satellite-Hybrid Management System — headend sender installer
#
#   curl -fsSL https://raw.githubusercontent.com/caritechsolutions/Satellite-Hybrid-Management-System/main/install.sh | sudo bash
#
# Builds the bundled modified librist (VSF TR-06-4 Part 6/7), the marker tools,
# and deploys the web management interface.
#
# Re-runnable: pulls latest code, preserves channel config.
# Env: BRANCH=<branch>  PORT=<http port>  SKIP_LIBRIST=1
#
set -e

REPO="https://github.com/caritechsolutions/Satellite-Hybrid-Management-System.git"
BRANCH="${BRANCH:-main}"
APP_DIR="/opt/shms"
WEB_ROOT="${APP_DIR}/rist-monitor"
SITE_NAME="rist-monitor"
LOG_DIR="/var/log/rist-monitor"
PORT="${PORT:-80}"

# Files that hold runtime state and must survive a code update
STATE_FILES="rist-monitor/config/transports.json rist-monitor/config/channels.json rist-monitor/config/satellites.json rist-monitor/data/receivers.json"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    WARNING: %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m    ERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root (use sudo)"

say "Satellite-Hybrid Management System — installer"
info "branch : ${BRANCH}"
info "target : ${APP_DIR}"

# ---------------------------------------------------------------- packages
say "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    nginx git curl ca-certificates \
    php-fpm php-cli php-curl php-mbstring php-xml \
    gcc g++ meson ninja-build pkg-config \
    libmbedtls-dev libcjson-dev libmicrohttpd-dev \
    ffmpeg >/dev/null
info "web stack + build toolchain + ffmpeg installed"

PHP_VER="$(ls /etc/php 2>/dev/null | sort -V | tail -1)"
[ -n "$PHP_VER" ] || die "no PHP found under /etc/php"
PHP_SOCK="/run/php/php${PHP_VER}-fpm.sock"
info "php    : ${PHP_VER} (${PHP_SOCK})"

DISABLED="$(php -r 'echo ini_get("disable_functions");' 2>/dev/null || true)"
case "$DISABLED" in
    *proc_open*|*exec*) warn "php disable_functions blocks proc_open/exec — channels will not start" ;;
esac

# ---------------------------------------------------------------- code
say "Fetching application code"
# Older installs chowned the whole tree (including .git) to www-data, which
# makes root's git refuse with "dubious ownership". Whitelist system-wide:
# --system writes /etc/gitconfig, so it does not depend on $HOME being set
# (which it is not, reliably, under `curl | sudo bash`).
if ! git config --system --get-all safe.directory 2>/dev/null | grep -qx "$APP_DIR"; then
    git config --system --add safe.directory "$APP_DIR" \
        || warn "could not add safe.directory exception"
fi

BACKUP="$(mktemp -d)"
if [ -d "${APP_DIR}/.git" ]; then
    for f in $STATE_FILES; do
        if [ -f "${APP_DIR}/${f}" ]; then
            mkdir -p "${BACKUP}/$(dirname "$f")"
            cp "${APP_DIR}/${f}" "${BACKUP}/${f}"
        fi
    done
    info "preserved existing config"
    git -C "$APP_DIR" fetch --quiet origin "$BRANCH"
    git -C "$APP_DIR" reset --hard --quiet "origin/${BRANCH}"
    git -C "$APP_DIR" checkout --quiet -B "$BRANCH" "origin/${BRANCH}"
else
    rm -rf "$APP_DIR"
    git clone --quiet --branch "$BRANCH" "$REPO" "$APP_DIR"
fi
info "at $(git -C "$APP_DIR" rev-parse --short HEAD)"

for f in $STATE_FILES; do
    [ -f "${BACKUP}/${f}" ] && cp "${BACKUP}/${f}" "${APP_DIR}/${f}"
done
rm -rf "$BACKUP"

[ -d "$WEB_ROOT" ] || die "expected ${WEB_ROOT} — wrong branch?"

# ---------------------------------------------------------------- librist
if [ "${SKIP_LIBRIST:-0}" = "1" ]; then
    say "Skipping librist build (SKIP_LIBRIST=1)"
elif [ ! -d "${APP_DIR}/librist" ]; then
    warn "librist/ not in the repo — skipping build"
else
    say "Building bundled librist (Part 6/7 modified)"
    cd "${APP_DIR}/librist"
    rm -rf build
    meson setup build \
        --prefix=/usr/local \
        --buildtype=release \
        -Dbuiltin_cjson=false \
        -Dtest=false >/dev/null
    ninja -C build >/dev/null
    ninja -C build install >/dev/null
    ldconfig
    [ -x /usr/local/bin/ristsender ] || die "ristsender missing after build"
    info "librist installed to /usr/local"
    info "ristsender + ristreceiver in /usr/local/bin"
fi

# ---------------------------------------------------------------- tools
say "Building marker tools"
cd "$APP_DIR"
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"

build_tool() {
    src="$1"; out="$2"; libs="$3"
    if [ ! -f "$src" ]; then warn "${src} missing - skipped"; return; fi
    # shellcheck disable=SC2086
    if gcc -O2 -Wall -pthread -I/usr/local/include -o "/usr/local/bin/${out}" \
           "$src" -L/usr/local/lib $libs 2>"/tmp/${out}.err"; then
        chmod +x "/usr/local/bin/${out}"
        info "built ${out}"
    else
        warn "${out} failed to build - see /tmp/${out}.err"
    fi
}

# Headend: converts the weight-0 RIST peer back to TS with Part 7 markers
build_tool ristreceiver_with_markers.c ristreceiver_with_markers "-lrist -lpthread"

# Headend, PSI-excluded variant of the above. Same job, but non_null counts only
# the marker plus elementary streams -- PSI (0x0000-0x001F plus the PMT PID) is
# forwarded on the wire as before but left out of the count, because PSI is
# regenerated by the third-party mux and re-injected by the STB capture and so
# can never agree between the two ends. Named for where it runs and what it does;
# the legacy name above is kept in place and untouched.
build_tool headend_part7_sender.c      headend_part7_sender      "-lrist -lpthread"
build_tool rist_watchdog.c             rist_watchdog             ""
build_tool ristsender_marker.c         ristsender_marker         "-lrist -lpthread"

# Part 8 (out-of-band) recovery server. Separate binary on purpose: librist keeps
# FSR state in udp.c file-scope statics, so running this in its own process is
# what stops it disturbing the Part 6/7 units above. Ingests the full downlink
# multiplex and indexes PCR -> RTP sequence for PCR-addressed recovery.
build_tool part8_recovery_server.c     part8_recovery_server     "-lrist -lpthread -lm"

# STB-side Part 7 receiver: validates the markers headend_part7_sender inserts,
# counting elementary streams only and rebuilding each block to 35 packets. This
# runs on the BOX (ARM) -- built here only so a compile break is caught on the
# headend; the shipping binary comes from the ARM cross-build.
build_tool stb_part7_receiver.c        stb_part7_receiver        "-lrist -lpthread"

# ---------------------------------------------------------------- tsduck
# TSDuck is used to post-process the marked TS before the uplink: declare the
# marker PID in the PMT, and remap PIDs. Installed from a prebuilt .deb rather
# than built from source - no toolchain, no long compile.
say "Installing TSDuck"
if command -v tsp >/dev/null 2>&1 && tsp --version >/dev/null 2>&1; then
    info "already installed: $(tsp --version 2>&1 | head -1)"
else
    # A broken install (present but not runnable) is worse than none - clear it
    if command -v tsp >/dev/null 2>&1; then
        warn "tsp present but not running - purging before reinstall"
        dpkg --purge tsduck >/dev/null 2>&1 || true
    fi

    ARCH="$(dpkg --print-architecture)"
    . /etc/os-release
    info "host: ${ARCH} on ${PRETTY_NAME:-unknown}"

    # Ubuntu 24 packages use libssl3t64/libcurl4t64 and will NOT work on older releases
    case "${VERSION_CODENAME:-}" in
        noble|plucky|oracular) TSD_VER="3.43-4524"; TSD_TAG="ubuntu24" ;;
        jammy)                 TSD_VER="3.33-3139"; TSD_TAG="ubuntu22" ;;
        focal)                 TSD_VER="3.26-2349"; TSD_TAG="ubuntu20" ;;
        *) warn "unrecognised release - trying the ubuntu24 package"
           TSD_VER="3.43-4524"; TSD_TAG="ubuntu24" ;;
    esac

    TSD_DEB=""
    LOCAL_DEB="$(find "${APP_DIR}/packages" -name "tsduck*${TSD_TAG}*${ARCH}.deb" 2>/dev/null | head -1)"
    if [ -n "$LOCAL_DEB" ]; then
        info "using bundled package: $(basename "$LOCAL_DEB")"
        TSD_DEB="$LOCAL_DEB"
    else
        TSD_PKG="tsduck_${TSD_VER}.${TSD_TAG}_${ARCH}.deb"
        info "no bundled package for ${TSD_TAG}/${ARCH} - downloading ${TSD_VER}"
        TSD_DEB="/tmp/tsduck.deb"; rm -f "$TSD_DEB"
        for attempt in 1 2 3; do
            if curl -fsSL -o "$TSD_DEB" \
               "https://github.com/tsduck/tsduck/releases/download/v${TSD_VER}/${TSD_PKG}"; then
                break
            fi
            warn "download attempt ${attempt} failed"; sleep 2
        done
    fi

    if [ -s "$TSD_DEB" ]; then
        apt-get install -y -qq libcurl4 libpcsclite1 libedit2 >/dev/null 2>&1 || true
        dpkg -i "$TSD_DEB" >/dev/null 2>&1 || warn "dpkg reported issues - fixing dependencies"
        apt-get install -f -y -qq >/dev/null 2>&1 || true
        [ "$TSD_DEB" = "/tmp/tsduck.deb" ] && rm -f /tmp/tsduck.deb
    else
        warn "no TSDuck package available"
        warn "  download tsduck_${TSD_VER}.${TSD_TAG}_${ARCH}.deb from"
        warn "  https://github.com/tsduck/tsduck/releases and put it in ${APP_DIR}/packages/"
    fi

    if command -v tsp >/dev/null 2>&1 && tsp --version >/dev/null 2>&1; then
        info "installed: $(tsp --version 2>&1 | head -1)"
    else
        warn "TSDuck not available - PMT/PID post-processing will not run"
    fi
fi

# ---------------------------------------------------------------- dirs/perms
say "Setting up directories and permissions"
mkdir -p "${WEB_ROOT}/config" "${WEB_ROOT}/data" "$LOG_DIR"
touch "${LOG_DIR}/rist-monitor.log"

# The code stays root-owned so git never sees "dubious ownership" (and the
# web user cannot rewrite its own application). Only the state directories
# and the log are handed to www-data.
chown -R root:root "$APP_DIR"
find "$APP_DIR" -type d -exec chmod 755 {} \;
find "$APP_DIR" -type f -exec chmod 644 {} \;

chown -R www-data:www-data "${WEB_ROOT}/config" "${WEB_ROOT}/data" "$LOG_DIR"
chmod 775 "${WEB_ROOT}/config" "${WEB_ROOT}/data"
chmod 664 "${WEB_ROOT}"/config/*.json "${WEB_ROOT}"/data/*.json 2>/dev/null || true
chmod 775 "$LOG_DIR"; chmod 664 "${LOG_DIR}/rist-monitor.log"
info "code root-owned; config/, data/ and logs writable by www-data"

# The UI manages channels as systemd units. Rather than granting www-data a
# broad root cp/rm, a narrow helper installs only ristsender-*/ristmarker-* units.
say "Installing systemd unit helper"
cat > /usr/local/sbin/rist-unit <<'HELPER'
#!/bin/bash
# rist-unit - install/remove RIST channel systemd units (root helper for the web UI)
set -e
ACTION="$1"; NAME="$2"; SRC="$3"

case "$NAME" in
    ristsender-*|ristmarker-*) ;;
    *) echo "refusing unit name: $NAME" >&2; exit 1 ;;
esac
case "$NAME" in
    */*|*..*) echo "refusing path traversal: $NAME" >&2; exit 1 ;;
esac

DEST="/etc/systemd/system/${NAME}.service"
case "$ACTION" in
    install)
        [ -f "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }
        install -m 0644 -o root -g root "$SRC" "$DEST"
        ;;
    remove)
        rm -f "$DEST"
        ;;
    *)
        echo "usage: rist-unit install|remove <unit-name> [source-file]" >&2; exit 1 ;;
esac
systemctl daemon-reload
HELPER
chmod 755 /usr/local/sbin/rist-unit
info "helper installed at /usr/local/sbin/rist-unit"

say "Granting systemd control to www-data"
cat > /etc/sudoers.d/rist-monitor <<'SUDO'
# Allow the web UI to manage RIST channel services only
www-data ALL=(root) NOPASSWD: /usr/local/sbin/rist-unit, /usr/bin/systemctl start ristsender-*, /usr/bin/systemctl stop ristsender-*, /usr/bin/systemctl restart ristsender-*, /usr/bin/systemctl enable ristsender-*, /usr/bin/systemctl disable ristsender-*, /usr/bin/systemctl is-active ristsender-*, /usr/bin/systemctl start ristmarker-*, /usr/bin/systemctl stop ristmarker-*, /usr/bin/systemctl restart ristmarker-*, /usr/bin/systemctl enable ristmarker-*, /usr/bin/systemctl disable ristmarker-*, /usr/bin/systemctl is-active ristmarker-*, /usr/bin/systemctl daemon-reload
SUDO
chmod 440 /etc/sudoers.d/rist-monitor
visudo -cf /etc/sudoers.d/rist-monitor >/dev/null || die "sudoers drop-in invalid"
info "www-data may manage ristsender-* / ristmarker-* units"

# ---------------------------------------------------------------- mcast bridge
# Internal multicast fabric: 238.0.0.0/8 routed to a member-less bridge, so a
# stream handed between local processes never touches a physical NIC. TSDuck's
# "-I ip" rejects a unicast address, so the internal hops need a real group.
say "Setting up the internal multicast bridge"
cat > /etc/systemd/system/rist-mcast-bridge.service <<'BRIDGE'
[Unit]
Description=Internal multicast bridge for RIST inter-process links
After=network-pre.target
Wants=network-pre.target
Before=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'ip link show br-rist >/dev/null 2>&1 || ip link add name br-rist type bridge'
ExecStart=/bin/sh -c 'ip link set br-rist up'
ExecStart=/bin/sh -c 'ip link set br-rist multicast on'
ExecStart=/bin/sh -c 'ip addr show dev br-rist | grep -q "10.255.255.1/24" || ip addr add 10.255.255.1/24 dev br-rist'
ExecStart=/bin/sh -c 'ip route show 238.0.0.0/8 2>/dev/null | grep -q br-rist || ip route add 238.0.0.0/8 dev br-rist'
ExecStop=/bin/sh -c 'ip route del 238.0.0.0/8 dev br-rist 2>/dev/null || true'
ExecStop=/bin/sh -c 'ip link del br-rist 2>/dev/null || true'
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
BRIDGE
systemctl daemon-reload
systemctl enable --now rist-mcast-bridge >/dev/null 2>&1 || warn "bridge unit did not start"
if ip route show 238.0.0.0/8 2>/dev/null | grep -q br-rist; then
    info "br-rist up, 238.0.0.0/8 routed to it (reserved - do not use elsewhere)"
else
    warn "238.0.0.0/8 route not present - the tsp stage will not receive"
fi

# ------------------------------------------------------- Part 8 recovery server
# A NEW, SEPARATE sender. It must not perturb the Part 6/7 chains, so:
#
#   - the unit is named part8-recovery, which deliberately does NOT match the
#     ristsender-*/ristmarker-* prefixes the rist-unit helper and the www-data
#     sudoers drop-in accept. The web UI therefore cannot start, stop, enable or
#     replace it, by construction rather than by convention.
#   - it has its own listen port, its own debug socket under /run, and its own
#     journal identity. Nothing is shared with an existing unit.
#   - no existing unit file is written, and no existing unit's enable/disable
#     state is touched. Known oddities (the stale ristmarker-bbc reference, the
#     disabled ristsender-vacation) are left exactly as they are.
#
# Isolation is VERIFIED below, not asserted: unit state is captured before and
# after and the difference is printed.
say "Part 8 recovery server"

P8_UNIT="part8-recovery"
P8_ENV="/etc/default/part8-recovery"
P8_STATE_BEFORE="/tmp/part8-units-before.txt"
P8_STATE_AFTER="/tmp/part8-units-after.txt"

# --- isolation capture, BEFORE
# --plain --no-legend --all so the comparison is over state, not over formatting,
# and so a unit that is loaded-but-inactive still appears.
capture_rist_state() {
    {
        systemctl list-units --type=service --all --plain --no-legend \
            'rist*' 'part8*' 2>/dev/null | awk '{print $1, $2, $3, $4}'
        echo "---enabled-state---"
        systemctl list-unit-files --no-legend 'rist*' 'part8*' 2>/dev/null \
            | awk '{print $1, $2}'
        echo "---main-pids---"
        for u in $(systemctl list-units --type=service --all --plain --no-legend 'rist*' 2>/dev/null | awk '{print $1}'); do
            printf '%s %s\n' "$u" "$(systemctl show -p MainPID --value "$u" 2>/dev/null)"
        done
    } | sort
}
capture_rist_state > "$P8_STATE_BEFORE"
info "captured pre-install unit state ($(wc -l < "$P8_STATE_BEFORE") lines) -> ${P8_STATE_BEFORE}"

# --- receive buffer ceiling
# Without this the server's 32 MB SO_RCVBUF request is silently clamped to 8 MB
# and the ingest drops under burst. Written as a sysctl drop-in so it survives a
# reboot, then applied now.
cat > /etc/sysctl.d/99-part8-recovery.conf <<'SYSCTL'
# Part 8 recovery server ingests a full ~59 Mb/s multiplex; the default
# rmem_max clamps its SO_RCVBUF request to 8 MB and input is lost on burst.
net.core.rmem_max = 33554432
SYSCTL
sysctl -q -w net.core.rmem_max=33554432 2>/dev/null || warn "could not set rmem_max now"
info "net.core.rmem_max = $(cat /proc/sys/net/core/rmem_max)"

# --- configuration, preserved across re-runs like the other state files
if [ -f "$P8_ENV" ]; then
    info "keeping existing ${P8_ENV}"
else
    cat > "$P8_ENV" <<'P8ENV'
# Part 8 recovery server configuration.
#
# P8_INPUT must be the LIVE TRANSPONDER FEED as raw TS over UDP. The unit will
# not start until it is set to something other than the placeholder below.
#   examples:  udp://@239.10.0.1:5000     (multicast, joins the group)
#              udp://@:5000               (unicast, any local address)
P8_INPUT="UNCONFIGURED"

# NOTE: values are QUOTED. The listen URL contains an ampersand, and this file
# is read both by systemd's EnvironmentFile and by a shell `.` in install.sh --
# unquoted, the shell would fork at the & and the variable would come back EMPTY.
# RIST listen address for recovery peers. weight=1000 is REQUIRED, not
# cosmetic: children inherit it, and weight-1000 peers are the ones that sit in
# the FSR-gated branch. With any other weight this server would emit
# continuously instead of staying silent until asked.
P8_LISTEN="rist://@0.0.0.0:9800?weight=1000&buffer=4000"

# Retransmit buffer target, milliseconds. 4000 is set on the RTT budget alone.
# It is NOT a switchover-gap lever: FSR resumes at the live edge with no
# backfill, so enlarging this cannot shrink a switchover gap. See
# docs/part8-milestone2-design-notes.md before changing it.
P8_BUFFER_MS=4000

# SO_RCVBUF request in bytes. Needs net.core.rmem_max at least this high.
P8_RCVBUF=33554432

# Extra flags. Leave EMPTY for production.
#   -S disables the require-selection policy and lets a peer with no registered
#      content selection pull the entire multiplex. Do not set it here.
P8_EXTRA=
P8ENV
    info "wrote ${P8_ENV} (P8_INPUT is UNCONFIGURED - set it before the unit will start)"
fi

# shellcheck disable=SC1090
. "$P8_ENV"

install -d -m 0755 /run/part8-recovery

cat > "/etc/systemd/system/${P8_UNIT}.service" <<'P8UNIT'
[Unit]
Description=VSF TR-06-4 Part 8 recovery server (PCR-addressed retransmission)
Documentation=https://github.com/caritechsolutions/Satellite-Hybrid-Management-System
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
EnvironmentFile=/etc/default/part8-recovery
RuntimeDirectory=part8-recovery
RuntimeDirectoryMode=0755

# Refuse to start rather than busy-loop against a placeholder address.
ExecStartPre=/bin/sh -c '[ "$P8_INPUT" != "UNCONFIGURED" ] || { echo "P8_INPUT not set in /etc/default/part8-recovery"; exit 1; }'

ExecStart=/bin/sh -c 'exec /usr/local/bin/part8_recovery_server \
    -i "$P8_INPUT" \
    -o "$P8_LISTEN" \
    -b "$P8_BUFFER_MS" \
    -r "$P8_RCVBUF" \
    -d /run/part8-recovery/debug.sock \
    $P8_EXTRA'

# D1 (blocking accept in the debug thread) is fixed, so SIGTERM is honoured and
# shutdown completes in a few milliseconds. The short timeout is deliberate: if
# this ever needs the full grace period again, the hang has returned.
KillSignal=SIGTERM
KillMode=mixed
TimeoutStopSec=10

Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal
SyslogIdentifier=part8-recovery

[Install]
WantedBy=multi-user.target
P8UNIT

systemctl daemon-reload

if [ "${P8_INPUT:-UNCONFIGURED}" = "UNCONFIGURED" ]; then
    warn "part8-recovery installed but NOT started: P8_INPUT is unset"
    warn "  set it in ${P8_ENV}, then: systemctl enable --now ${P8_UNIT}"
else
    systemctl enable "$P8_UNIT" >/dev/null 2>&1 || warn "could not enable ${P8_UNIT}"
    systemctl restart "$P8_UNIT" || warn "${P8_UNIT} did not start"
    sleep 3
    if systemctl is-active --quiet "$P8_UNIT"; then
        info "${P8_UNIT} active, input ${P8_INPUT}, listen ${P8_LISTEN}"
        info "achieved SO_RCVBUF (from its own startup log):"
        journalctl -u "$P8_UNIT" -n 200 --no-pager 2>/dev/null \
            | grep -iE "SO_RCVBUF|rmem" | tail -3 | sed 's/^/        /'
    else
        warn "${P8_UNIT} is not active - journalctl -u ${P8_UNIT} -n 50"
    fi
fi

# --- clean-stop check (D1 regression guard)
# A unit that needs SIGKILL at the timeout would show "state=timeout" or a
# non-zero code here. This is the check that D1 has not come back.
if systemctl is-active --quiet "$P8_UNIT"; then
    say "Verifying ${P8_UNIT} stops cleanly (D1 regression check)"
    _t0=$(date +%s%N)
    systemctl stop "$P8_UNIT"
    _t1=$(date +%s%N)
    _ms=$(( (_t1 - _t0) / 1000000 ))
    _res="$(systemctl show -p Result --value "$P8_UNIT" 2>/dev/null)"
    if [ "$_res" = "success" ] && [ "$_ms" -lt 5000 ]; then
        info "stopped in ${_ms} ms, Result=${_res} (no SIGKILL at timeout)"
    else
        warn "stop took ${_ms} ms, Result=${_res} - D1 may have regressed"
    fi
    journalctl -u "$P8_UNIT" -n 20 --no-pager 2>/dev/null \
        | grep -E "\[STOP\]" | sed 's/^/        /'
    systemctl start "$P8_UNIT"
    sleep 2
    systemctl is-active --quiet "$P8_UNIT" && info "restarted, still active"
fi

# --- long-run observer (O1-O5)
# Not a unit: it is started by hand for a measurement run and stopped when the
# run ends. Making it a service would mean another always-on process next to the
# thing being measured, which is the opposite of what an isolation-sensitive
# deployment wants.
if [ -f "${APP_DIR}/tests/part8/observe.py" ]; then
    install -m 0755 "${APP_DIR}/tests/part8/observe.py" /usr/local/bin/part8-observe
    install -d -m 0755 /var/log/part8-observe
    info "observer at /usr/local/bin/part8-observe (writes /var/log/part8-observe)"
    info "  run:  part8-observe --interval 30 &"
else
    warn "tests/part8/observe.py not in the repo - observer not installed"
fi

# --- isolation capture, AFTER, and the difference
capture_rist_state > "$P8_STATE_AFTER"
say "Isolation check: what changed in rist*/part8* unit state"
if diff -u "$P8_STATE_BEFORE" "$P8_STATE_AFTER" > /tmp/part8-units-diff.txt 2>&1; then
    info "NOTHING changed at all (part8-recovery was already in this state)"
else
    _touched="$(grep -E '^[+-][^+-]' /tmp/part8-units-diff.txt | grep -vE 'part8' || true)"
    if [ -z "$_touched" ]; then
        info "ONLY part8-recovery lines differ - no existing rist* unit changed"
        grep -E '^\+[^+]' /tmp/part8-units-diff.txt | sed 's/^/        /'
    else
        warn "an EXISTING unit changed - this should not happen:"
        printf '%s\n' "$_touched" | sed 's/^/        /'
    fi
    info "full diff: /tmp/part8-units-diff.txt"
fi
info "before: ${P8_STATE_BEFORE}   after: ${P8_STATE_AFTER}"

# ---------------------------------------------------------------- nginx
say "Configuring nginx"
cat > "/etc/nginx/sites-available/${SITE_NAME}" <<NGINX
server {
    listen ${PORT} default_server;
    listen [::]:${PORT} default_server;
    server_name _;
    root ${WEB_ROOT};
    index index.php index.html;
    charset utf-8;

    access_log /var/log/nginx/${SITE_NAME}.access.log;
    error_log  /var/log/nginx/${SITE_NAME}.error.log;

    location / { try_files \$uri \$uri/ /index.php?\$query_string; }

    location ~ \.php\$ {
        include snippets/fastcgi-php.conf;
        fastcgi_pass unix:${PHP_SOCK};
        fastcgi_read_timeout 120;
    }

    location /assets/ { expires 7d; add_header Cache-Control "public"; }

    location ~ ^/(config|data)/ { deny all; }
    location ~ /\.git { deny all; }
    location ~ /\.    { deny all; }

    add_header X-Frame-Options "SAMEORIGIN";
    add_header X-Content-Type-Options "nosniff";
}
NGINX

rm -f /etc/nginx/sites-enabled/default
ln -sf "/etc/nginx/sites-available/${SITE_NAME}" "/etc/nginx/sites-enabled/${SITE_NAME}"
nginx -t >/dev/null 2>&1 || die "nginx config test failed - run 'nginx -t'"
systemctl enable --now nginx >/dev/null 2>&1 || true
systemctl enable --now "php${PHP_VER}-fpm" >/dev/null 2>&1 || true
systemctl reload nginx
info "site live on port ${PORT}"

# ---------------------------------------------------------------- done
IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
say "Done"
if [ "$PORT" = "80" ]; then URL="http://${IP:-<server-ip>}/"; else URL="http://${IP:-<server-ip>}:${PORT}/"; fi
info "web ui   : ${URL}"
info "code     : ${APP_DIR} (git ${BRANCH})"
info "binaries : /usr/local/bin/{ristsender,ristreceiver,ristreceiver_with_markers,headend_part7_sender,part8_recovery_server,rist_watchdog}"
info "tsduck   : $(command -v tsp >/dev/null 2>&1 && tsp --version 2>&1 | head -1 || echo \"not installed\")"
info "app log  : ${LOG_DIR}/rist-monitor.log"
info "unit log : journalctl -u ristsender-<channel> -f"
echo
info "This server's IP is ${IP:-unknown} - the sender advertises it for both"
info "the weight-0 and weight-1000 peers."
info "Add your admin IP to ALLOWED_IPS in rist-monitor/config/config.php."
echo
