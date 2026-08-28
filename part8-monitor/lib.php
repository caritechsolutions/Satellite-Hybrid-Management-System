<?php
/**
 * Part 8 management UI - instance store, port allocation, unit control.
 *
 * Talks to systemd ONLY through /usr/local/sbin/part8-unit, which constructs
 * part8-recovery@<token> itself. Nothing here can name a Part 7 unit, and this
 * app is granted no systemctl rights of its own.
 */

const P8_HELPER     = '/usr/local/sbin/part8-unit';
const P8_STORE      = __DIR__ . '/config/instances.json';
const P8_ENV_DIR    = '/etc/part8/instances';
const P8_RUN_DIR    = '/run/part8-recovery';

/* Port block. Verified rather than assumed when this was chosen: the box's
 * ephemeral range is 32768-60999, /etc/services claims nothing in 9800-9899,
 * and every Part 7 port in use (5000, 5554, 5555, 8000, 11304, 11450, 12050)
 * lies outside it. p8_port_conflicts() re-checks all of that at allocation
 * time rather than trusting this comment. */
const P8_PORT_MIN   = 9800;
const P8_PORT_MAX   = 9899;

const P8_DEFAULT_BUFFER_MS = 4000;
const P8_DEFAULT_RCVBUF    = 33554432;

function p8_instances(): array
{
    if (!is_file(P8_STORE)) return [];
    $j = json_decode((string)file_get_contents(P8_STORE), true);
    return is_array($j) ? $j : [];
}

function p8_save(array $list): bool
{
    $dir = dirname(P8_STORE);
    if (!is_dir($dir)) @mkdir($dir, 0775, true);
    $tmp = P8_STORE . '.tmp';
    if (file_put_contents($tmp, json_encode($list, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES)) === false)
        return false;
    return rename($tmp, P8_STORE);
}

function p8_valid_name(string $n): bool
{
    return (bool)preg_match('/^[a-z0-9][a-z0-9-]{0,30}$/', $n);
}

/* ------------------------------------------------------------- port rules */

/**
 * Every port we must not collide with, and why. Four independent sources,
 * because each misses what the others catch:
 *
 *   1. our own instance registry   - two Part 8 instances must not share
 *   2. Part 7 unit files           - a unit that is currently STOPPED still
 *                                    owns its port and will want it back
 *   3. the live socket tables      - anything actually bound right now,
 *                                    including processes we know nothing about
 *   4. the instance's own input    - the input is a UDP listener too, and a
 *                                    listen/input clash is the one people miss
 */
function p8_whoami(): string
{
    if (function_exists('posix_geteuid') && function_exists('posix_getpwuid')) {
        $pw = @posix_getpwuid(posix_geteuid());
        if (is_array($pw) && isset($pw['name'])) return $pw['name'];
    }
    return (string)(getenv('USER') ?: 'the web user');
}

function p8_ports_in_use(?string $exclude = null, ?array &$problems = null): array
{
    $used = [];
    if ($problems === null) $problems = [];

    // 1. our own registry
    foreach (p8_instances() as $name => $inst) {
        if ($exclude !== null && $name === $exclude) continue;
        if (!empty($inst['listen_port'])) $used[(int)$inst['listen_port']] = "part8 instance '$name'";
        $ip = p8_port_of_url($inst['input'] ?? '');
        if ($ip) $used[$ip] = "part8 instance '$name' input";
    }

    // 2. Part 7 unit files -- parse ExecStart of every unit we do not own.
    //    A unit that systemd knows about but which has NO unit file (a stale
    //    not-found reference like ristmarker-bbc.service) simply does not appear
    //    here, which is correct: it owns no ports because it cannot start.
    if (!is_readable('/etc/systemd/system')) {
        $problems[] = '/etc/systemd/system is not readable as ' . p8_whoami()
                    . ' -- Part 7 ports cannot be checked';
    }
    foreach (glob('/etc/systemd/system/rist*.service') ?: [] as $f) {
        if (!is_readable($f)) {
            $problems[] = "$f is not readable as " . p8_whoami();
            continue;
        }
        $body = (string)@file_get_contents($f);
        if ($body === '') continue;
        $unit = basename($f);
        if (preg_match_all('/:(\d{2,5})\b/', $body, $m)) {
            foreach ($m[1] as $p) {
                $p = (int)$p;
                if ($p >= 1 && $p <= 65535 && !isset($used[$p])) $used[$p] = "Part 7 unit $unit";
            }
        }
    }

    // 2b. the SHMS channel config, for channels whose units are not written yet
    foreach (['/opt/shms/rist-monitor/config/channels.json',
              '/opt/shms/rist-monitor/config/transports.json'] as $cf) {
        if (!file_exists($cf)) continue;               // absent is fine
        if (!is_readable($cf)) {                        // present but unreadable is NOT
            $problems[] = "$cf exists but is not readable as " . p8_whoami();
            continue;
        }
        $raw = file_get_contents($cf);
        if ($raw === false) { $problems[] = "could not read $cf"; continue; }
        $decoded = json_decode((string)$raw, true);
        if (!is_array($decoded)) {
            $problems[] = "$cf is not valid JSON (" . json_last_error_msg() . ")";
            continue;
        }
        // array_walk_recursive takes its first argument BY REFERENCE, so it must
        // be a variable. Passing `$j ?: []` is a fatal at runtime, not a parse
        // error, which is why php -l passed and why this only fired on a host
        // where the file actually exists.
        array_walk_recursive($decoded, function ($v, $k) use (&$used) {
            if (is_numeric($v) && stripos((string)$k, 'port') !== false) {
                $p = (int)$v;
                if ($p >= 1 && $p <= 65535 && !isset($used[$p])) $used[$p] = 'SHMS channel config';
            }
        });
    }

    // 3. anything actually bound, per the kernel
    $sawSocketTable = false;
    foreach (['/proc/net/udp', '/proc/net/udp6', '/proc/net/tcp', '/proc/net/tcp6'] as $pf) {
        if (!file_exists($pf)) continue;                 // IPv6 disabled, say
        if (!is_readable($pf)) {
            $problems[] = "$pf is not readable as " . p8_whoami();
            continue;
        }
        $lines = @file($pf, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
        if (!$lines) continue;
        $sawSocketTable = true;
        foreach (array_slice($lines, 1) as $l) {
            $f = preg_split('/\s+/', trim($l));
            if (count($f) < 2 || strpos($f[1], ':') === false) continue;
            $p = hexdec(substr($f[1], strpos($f[1], ':') + 1));
            if ($p >= 1 && $p <= 65535 && !isset($used[$p])) $used[$p] = 'already bound';
        }
    }

    if (!$sawSocketTable)
        $problems[] = 'no /proc/net socket table could be read -- ports already '
                    . 'bound by other processes cannot be seen';

    return $used;
}

function p8_port_of_url(string $url): ?int
{
    if ($url === '' ) return null;
    if (preg_match('/:(\d{2,5})(?:[?\/]|$)/', $url, $m)) return (int)$m[1];
    return null;
}

/**
 * Refuse rather than guess. Returns [port, null] or [null, reason].
 *
 * If any source that could hold a conflicting port could not be read, this
 * REFUSES rather than allocating. Handing out a port while blind to one of the
 * four sources is how a Part 8 instance would end up on a Part 7 port -- the
 * exact failure the rule exists to prevent -- and it would look like a working
 * allocation until something collided at runtime.
 */
function p8_alloc_port(?string $exclude = null): array
{
    $problems = [];
    $used = p8_ports_in_use($exclude, $problems);
    if ($problems) {
        return [null, "cannot allocate a port safely:\n  - " . implode("\n  - ", $problems)
                    . "\nRefusing rather than risk a collision with a Part 7 port."];
    }

    // The ephemeral range can be reconfigured; re-read it instead of trusting
    // the constant chosen when this was written.
    $eph = @file_get_contents('/proc/sys/net/ipv4/ip_local_port_range');
    $lo = $hi = 0;
    if ($eph && preg_match('/(\d+)\s+(\d+)/', $eph, $m)) { $lo = (int)$m[1]; $hi = (int)$m[2]; }

    for ($p = P8_PORT_MIN; $p <= P8_PORT_MAX; $p++) {
        if (isset($used[$p])) continue;
        if ($lo && $p >= $lo && $p <= $hi) continue;   // inside the ephemeral range
        return [$p, null];
    }
    return [null, sprintf(
        'no free port in %d-%d (%d of %d taken). Refusing rather than reusing one.',
        P8_PORT_MIN, P8_PORT_MAX,
        count(array_filter(array_keys($used), fn($p) => $p >= P8_PORT_MIN && $p <= P8_PORT_MAX)),
        P8_PORT_MAX - P8_PORT_MIN + 1)];
}

/**
 * Readability of everything the allocator depends on, for the diag endpoint.
 * Reported rather than assumed: this app runs as www-data on a production host,
 * not as root in a container, and that difference is what broke the first run.
 */
function p8_diagnostics(): array
{
    $out = ['user' => p8_whoami(), 'sources' => [], 'problems' => []];
    $paths = [
        '/etc/systemd/system'                              => 'Part 7 unit files (directory)',
        '/proc/net/udp'                                    => 'bound UDP sockets',
        '/proc/net/tcp'                                    => 'bound TCP sockets',
        '/proc/sys/net/ipv4/ip_local_port_range'           => 'ephemeral port range',
        '/opt/shms/rist-monitor/config/channels.json'      => 'SHMS channel config',
        P8_ENV_DIR                                         => 'instance env files',
        P8_RUN_DIR                                         => 'instance debug sockets',
        dirname(P8_STORE)                                  => 'instance store (must be WRITABLE)',
        P8_HELPER                                          => 'privilege helper',
    ];
    foreach ($paths as $p => $what) {
        $out['sources'][] = [
            'path' => $p, 'purpose' => $what,
            'exists' => file_exists($p),
            'readable' => is_readable($p),
            'writable' => is_writable($p),
        ];
    }
    $problems = [];
    p8_ports_in_use(null, $problems);
    $out['problems'] = $problems;

    // The helper is the only privileged path; prove it answers before an
    // operator discovers otherwise by clicking Start.
    $h = p8_helper('list');
    $out['helper'] = ['rc' => $h['rc'], 'output' => substr($h['out'], 0, 400)];
    return $out;
}

/** Explicit check for a user-supplied port. Returns null if free, else why not. */
function p8_port_conflicts(int $port, ?string $exclude = null): ?string
{
    if ($port < 1 || $port > 65535) return 'out of range';
    $problems = [];
    $used = p8_ports_in_use($exclude, $problems);
    if ($problems)
        return "cannot verify this port: " . implode('; ', $problems);
    if (isset($used[$port])) return $used[$port];
    if ($port < P8_PORT_MIN || $port > P8_PORT_MAX)
        return sprintf('outside the Part 8 block %d-%d', P8_PORT_MIN, P8_PORT_MAX);
    return null;
}

/* --------------------------------------------------------- unit control */

function p8_helper(string $action, string $name = '', string $stdin = '', string $extra = ''): array
{
    $cmd = 'sudo -n ' . escapeshellarg(P8_HELPER) . ' ' . escapeshellarg($action);
    if ($name !== '')  $cmd .= ' ' . escapeshellarg($name);
    if ($extra !== '') $cmd .= ' ' . escapeshellarg($extra);
    $cmd .= ' 2>&1';

    $desc = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $p = proc_open($cmd, $desc, $pipes);
    if (!is_resource($p)) return ['rc' => -1, 'out' => 'could not run helper'];
    if ($stdin !== '') fwrite($pipes[0], $stdin);
    fclose($pipes[0]);
    $out = stream_get_contents($pipes[1]) . stream_get_contents($pipes[2]);
    fclose($pipes[1]); fclose($pipes[2]);
    $rc = proc_close($p);
    return ['rc' => $rc, 'out' => trim((string)$out)];
}

function p8_env_body(array $inst): string
{
    // Values are QUOTED. The listen URL contains an ampersand, and this file is
    // read by systemd's EnvironmentFile and by a shell during install; unquoted,
    // a shell forks at the & and the variable comes back EMPTY.
    return
        "# Generated by the Part 8 management UI. Edited here or in the UI; the UI\n" .
        "# rewrites this file wholesale, so hand edits are lost on the next save.\n" .
        'P8_INPUT="'     . $inst['input'] . "\"\n" .
        'P8_LISTEN="'    . sprintf('rist://@0.0.0.0:%d?weight=1000&buffer=%d',
                                   $inst['listen_port'], $inst['buffer_ms']) . "\"\n" .
        'P8_BUFFER_MS='  . (int)$inst['buffer_ms'] . "\n" .
        'P8_RCVBUF='     . (int)$inst['rcvbuf'] . "\n" .
        'P8_EXTRA="'     . (empty($inst['require_selection']) ? '-S' : '') . "\"\n";
}

/* ------------------------------------------------------------- live stats */

/**
 * One round trip to an instance's debug socket for the whole JSON document.
 * Short timeouts throughout: a wedged instance must slow the panel down, not
 * hang it, and N instances are polled in sequence.
 */
function p8_stats(string $name, float $timeout = 2.0): ?array
{
    $path = P8_RUN_DIR . '/' . $name . '/debug.sock';
    if (!file_exists($path)) return null;
    $err = 0; $msg = '';
    $s = @stream_socket_client('unix://' . $path, $err, $msg, $timeout);
    if (!$s) return null;
    stream_set_timeout($s, (int)$timeout, (int)(($timeout - (int)$timeout) * 1e6));
    fwrite($s, "json\n");
    $buf = '';
    while (!feof($s)) {
        $chunk = fread($s, 65536);
        if ($chunk === false || $chunk === '') break;
        $buf .= $chunk;
        if (strlen($buf) > 4 * 1024 * 1024) break;      // refuse to grow forever
    }
    fclose($s);
    $j = json_decode($buf, true);
    return is_array($j) ? $j : null;
}

function p8_state(string $name): array
{
    $r = p8_helper('status', $name);
    $lines = preg_split('/\s+/', trim($r['out']));
    return [
        'active'  => $lines[0] ?? 'unknown',
        'enabled' => $lines[1] ?? 'unknown',
    ];
}
