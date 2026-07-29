#!/bin/bash
#
# Satellite-Hybrid Management System (RIST Monitor) — installer
#
#   curl -fsSL https://raw.githubusercontent.com/caritechsolutions/Satellite-Hybrid-Management-System/main/install.sh | sudo bash
#
# Re-runnable: pulls the latest code and preserves your channel/transport config.
# Env overrides:  BRANCH=<branch>  BUILD_LIBRIST=yes|no|auto  PORT=<http port>
#
set -e

REPO="https://github.com/caritechsolutions/Satellite-Hybrid-Management-System.git"
BRANCH="${BRANCH:-main}"
APP_DIR="/opt/shms"
WEB_ROOT="${APP_DIR}/rist-monitor"
SITE_NAME="rist-monitor"
LOG_DIR="/var/log/rist-monitor"
PORT="${PORT:-80}"
BUILD_LIBRIST="${BUILD_LIBRIST:-auto}"

# State files that must survive a code update (they live in the repo tree)
STATE_FILES="rist-monitor/config/transports.json rist-monitor/config/satellites.json rist-monitor/data/receivers.json"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m    WARNING: %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m    ERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root (use sudo)"

say "Satellite-Hybrid Management System — installer"
info "repo   : ${REPO}"
info "branch : ${BRANCH}"
info "target : ${APP_DIR}"

# ---------------------------------------------------------------- packages
say "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq nginx git curl ca-certificates \
    php-fpm php-cli php-curl php-mbstring php-xml >/dev/null
info "nginx, php-fpm, git installed"

# Detect the PHP-FPM version/socket actually installed
PHP_VER="$(ls /etc/php 2>/dev/null | sort -V | tail -1)"
[ -n "$PHP_VER" ] || die "no PHP installation found under /etc/php"
PHP_SOCK="/run/php/php${PHP_VER}-fpm.sock"
info "php    : ${PHP_VER}  (socket ${PHP_SOCK})"

# proc_open/exec are required to launch ristsender from the web UI
DISABLED="$(php -r 'echo ini_get("disable_functions");' 2>/dev/null || true)"
case "$DISABLED" in
    *proc_open*|*exec*) warn "php disable_functions blocks proc_open/exec — transports will not start" ;;
esac

# ---------------------------------------------------------------- code
say "Fetching application code"
BACKUP="$(mktemp -d)"
if [ -d "${APP_DIR}/.git" ]; then
    for f in $STATE_FILES; do
        if [ -f "${APP_DIR}/${f}" ]; then
            mkdir -p "${BACKUP}/$(dirname "$f")"
            cp "${APP_DIR}/${f}" "${BACKUP}/${f}"
        fi
    done
    info "preserved existing config/data"
    git -C "$APP_DIR" fetch --quiet origin "$BRANCH"
    git -C "$APP_DIR" checkout --quiet -B "$BRANCH" "origin/${BRANCH}"
    git -C "$APP_DIR" reset --hard --quiet "origin/${BRANCH}"
    info "updated to $(git -C "$APP_DIR" rev-parse --short HEAD)"
else
    rm -rf "$APP_DIR"
    git clone --quiet --branch "$BRANCH" "$REPO" "$APP_DIR"
    info "cloned at $(git -C "$APP_DIR" rev-parse --short HEAD)"
fi

# Restore preserved state over the fresh checkout
for f in $STATE_FILES; do
    [ -f "${BACKUP}/${f}" ] && cp "${BACKUP}/${f}" "${APP_DIR}/${f}"
done
rm -rf "$BACKUP"

[ -d "$WEB_ROOT" ] || die "expected ${WEB_ROOT} in the repo — wrong branch?"

# ---------------------------------------------------------------- dirs/perms
say "Setting up directories and permissions"
mkdir -p "${WEB_ROOT}/config" "${WEB_ROOT}/data" "$LOG_DIR"
touch "${LOG_DIR}/rist-monitor.log"

chown -R www-data:www-data "$APP_DIR" "$LOG_DIR"
find "$APP_DIR" -type d -exec chmod 755 {} \;
find "$APP_DIR" -type f -exec chmod 644 {} \;
chmod 775 "${WEB_ROOT}/config" "${WEB_ROOT}/data"
chmod 664 "${WEB_ROOT}"/config/*.json "${WEB_ROOT}"/data/*.json 2>/dev/null || true
chmod 775 "$LOG_DIR"; chmod 664 "${LOG_DIR}/rist-monitor.log"
info "owner www-data, config/ and data/ writable"

# ---------------------------------------------------------------- nginx
say "Configuring nginx"
cat > "/etc/nginx/sites-available/${SITE_NAME}" <<NGINX
server {
    listen ${PORT} default_server;
    listen [::]:${PORT} default_server;
    server_name _;
    root ${WEB_ROOT};
    index index.php index.html;

    access_log /var/log/nginx/${SITE_NAME}.access.log;
    error_log  /var/log/nginx/${SITE_NAME}.error.log;

    location / {
        try_files \$uri \$uri/ /index.php?\$query_string;
    }

    location ~ \.php\$ {
        include snippets/fastcgi-php.conf;
        fastcgi_pass unix:${PHP_SOCK};
        fastcgi_read_timeout 120;
    }

    location /assets/ {
        expires 7d;
        add_header Cache-Control "public";
    }

    # never serve config, data or the git metadata over http
    location ~ ^/(config|data)/ { deny all; }
    location ~ /\.git      { deny all; }
    location ~ /\.         { deny all; }

    add_header X-Frame-Options "SAMEORIGIN";
    add_header X-Content-Type-Options "nosniff";
}
NGINX

rm -f /etc/nginx/sites-enabled/default
ln -sf "/etc/nginx/sites-available/${SITE_NAME}" "/etc/nginx/sites-enabled/${SITE_NAME}"
nginx -t >/dev/null 2>&1 || die "nginx config test failed — run 'nginx -t' to see why"
systemctl enable --now nginx  >/dev/null 2>&1 || true
systemctl enable --now "php${PHP_VER}-fpm" >/dev/null 2>&1 || true
systemctl reload nginx
info "site enabled on port ${PORT}"

# ---------------------------------------------------------------- librist
say "Checking RIST binaries"
NEED_BUILD="no"
if [ -x /usr/local/bin/ristsender ]; then
    info "ristsender present: $(/usr/local/bin/ristsender --help 2>&1 | head -1 || echo ok)"
else
    case "$BUILD_LIBRIST" in
        yes|auto) NEED_BUILD="yes" ;;
        *) warn "ristsender not found and BUILD_LIBRIST=no — install it before starting a transport" ;;
    esac
fi

if [ "$NEED_BUILD" = "yes" ]; then
    info "building librist from source (this takes a few minutes)"
    apt-get install -y -qq build-essential meson ninja-build pkg-config cmake \
        libmbedtls-dev libcjson-dev >/dev/null
    TMP="$(mktemp -d)"
    git clone --quiet --depth 1 https://code.videolan.org/rist/librist.git "${TMP}/librist"
    ( cd "${TMP}/librist" \
      && meson setup build --prefix=/usr/local --buildtype=release >/dev/null \
      && ninja -C build >/dev/null \
      && ninja -C build install >/dev/null )
    ldconfig
    rm -rf "$TMP"
    [ -x /usr/local/bin/ristsender ] && info "librist installed to /usr/local/bin" \
        || warn "librist build finished but ristsender not found"
fi

# If a marker build is present, point it out — the app can be configured to use it
if [ -x /usr/local/bin/ristsender_marker ]; then
    info "ristsender_marker present — set RIST_SENDER_BINARY in config/config.php to use it"
fi

# ---------------------------------------------------------------- done
IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
say "Done"
info "URL       : http://${IP:-<server-ip>}${PORT:+$( [ "$PORT" = 80 ] && echo "" || echo ":$PORT" )}/"
info "code      : ${APP_DIR}   (git ${BRANCH})"
info "config    : ${WEB_ROOT}/config/transports.json"
info "app log   : ${LOG_DIR}/rist-monitor.log"
info "nginx log : /var/log/nginx/${SITE_NAME}.error.log"
echo
info "NOTE: config/config.php has ALLOWED_IPS — add this machine's admin IP"
info "      or the UI will refuse requests."
echo
