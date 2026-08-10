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
info "binaries : /usr/local/bin/{ristsender,ristreceiver,ristreceiver_with_markers,headend_part7_sender,rist_watchdog}"
info "tsduck   : $(command -v tsp >/dev/null 2>&1 && tsp --version 2>&1 | head -1 || echo \"not installed\")"
info "app log  : ${LOG_DIR}/rist-monitor.log"
info "unit log : journalctl -u ristsender-<channel> -f"
echo
info "This server's IP is ${IP:-unknown} - the sender advertises it for both"
info "the weight-0 and weight-1000 peers."
info "Add your admin IP to ALLOWED_IPS in rist-monitor/config/config.php."
echo
