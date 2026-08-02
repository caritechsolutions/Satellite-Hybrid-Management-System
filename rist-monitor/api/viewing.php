<?php
// api/viewing.php - Aggregated viewing statistics from the boxes
//
//   GET /api/viewing.php?range=1h|24h|7d|all   (default 24h)
//
// Reads DATA_DIR/stats/views.jsonl (append-only, one record per view) and
// returns summary KPIs plus per-channel, per-box and recent-view breakdowns.
// All timing is taken from `received_at` (server clock) rather than the box's
// own clock, which is unreliable when it has no satellite.

require_once dirname(__DIR__) . '/config/config.php';

if (!defined('STATS_DIR')) define('STATS_DIR', DATA_DIR . '/stats');
if (!defined('STATS_LOG')) define('STATS_LOG', STATS_DIR . '/views.jsonl');

// Cap how much of the log we parse so a long-running server can't blow memory.
if (!defined('VIEW_MAX_LINES')) define('VIEW_MAX_LINES', 20000);

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') { setCORSHeaders(); exit; }
setCORSHeaders();

function emit($payload, $status = 200)
{
    http_response_code($status);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($payload, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES);
    exit;
}

/** Read the last $max lines of a file without loading all of it. */
function tail_lines($path, $max)
{
    if (!is_readable($path)) return [];
    $size = filesize($path);
    if ($size === 0) return [];

    $fp = fopen($path, 'rb');
    if (!$fp) return [];

    $chunk = 65536;
    $pos   = $size;
    $buf   = '';
    $lines = 0;

    while ($pos > 0 && $lines <= $max) {
        $read = min($chunk, $pos);
        $pos -= $read;
        fseek($fp, $pos);
        $buf   = fread($fp, $read) . $buf;
        $lines = substr_count($buf, "\n");
    }
    fclose($fp);

    $all = explode("\n", $buf);
    if (count($all) > $max) $all = array_slice($all, -$max);
    return $all;
}

function range_seconds($r)
{
    switch ($r) {
        case '1h':  return 3600;
        case '24h': return 86400;
        case '7d':  return 604800;
        case 'all': return 0;
        default:    return 86400;
    }
}

function fmt_hms($ms)
{
    $s = (int)round($ms / 1000);
    $h = intdiv($s, 3600); $m = intdiv($s % 3600, 60); $sec = $s % 60;
    if ($h) return sprintf('%dh %dm', $h, $m);
    if ($m) return sprintf('%dm %ds', $m, $sec);
    return $sec . 's';
}

try {
    $range   = isset($_GET['range']) ? $_GET['range'] : '24h';
    $window  = range_seconds($range);
    $cutoff  = $window ? (time() - $window) : 0;

    $lines = tail_lines(STATS_LOG, VIEW_MAX_LINES);

    $summary = [
        'views' => 0, 'watch_ms' => 0, 'boxes' => 0,
        'rist_views' => 0, 'tuner_views' => 0,
        'failed_views' => 0, 'recovery_only_views' => 0,
    ];
    $channels = [];   // service_id => aggregate
    $boxes    = [];   // box_id     => aggregate
    $recent   = [];

    foreach ($lines as $line) {
        $line = trim($line);
        if ($line === '') continue;
        $r = json_decode($line, true);
        if (!is_array($r) || !isset($r['received_at'])) continue;

        $ts = strtotime($r['received_at']);
        if ($cutoff && $ts < $cutoff) continue;

        $sid   = isset($r['service_id']) ? (int)$r['service_id'] : 0;
        $box   = isset($r['box_id']) ? (string)$r['box_id'] : '?';
        $dur   = isset($r['duration_ms']) ? (int)$r['duration_ms'] : 0;
        $path  = isset($r['path']) ? (string)$r['path'] : '';
        $sat   = isset($r['sat_source']) ? (string)$r['sat_source'] : '';
        $ff    = isset($r['first_frame_ms']) ? (int)$r['first_frame_ms'] : -1;
        $name  = isset($r['name']) && $r['name'] !== '' ? (string)$r['name'] : ('svc ' . $sid);
        $failed = ($ff < 0);

        $summary['views']++;
        $summary['watch_ms'] += $dur;
        if ($path === 'rist')  $summary['rist_views']++;
        if ($path === 'tuner') $summary['tuner_views']++;
        if ($failed)           $summary['failed_views']++;
        if ($sat === 'none')   $summary['recovery_only_views']++;

        if (!isset($channels[$sid])) {
            $channels[$sid] = [
                'service_id' => $sid, 'name' => $name,
                'views' => 0, 'watch_ms' => 0, 'rist' => 0, 'tuner' => 0,
                'failed' => 0, 'recovery_only' => 0, 'ff_sum' => 0, 'ff_n' => 0,
            ];
        }
        $c = &$channels[$sid];
        $c['name'] = $name;
        $c['views']++; $c['watch_ms'] += $dur;
        if ($path === 'rist')  $c['rist']++;
        if ($path === 'tuner') $c['tuner']++;
        if ($failed)           $c['failed']++;
        if ($sat === 'none')   $c['recovery_only']++;
        if ($ff >= 0) { $c['ff_sum'] += $ff; $c['ff_n']++; }
        unset($c);

        if (!isset($boxes[$box])) {
            $boxes[$box] = [
                'box_id' => $box, 'views' => 0, 'watch_ms' => 0,
                'last_seen' => '', 'clock_synced' => false, 'rist' => 0,
            ];
        }
        $b = &$boxes[$box];
        $b['views']++; $b['watch_ms'] += $dur;
        if ($path === 'rist') $b['rist']++;
        if ($ts >= strtotime($b['last_seen'] ?: '@0')) {
            $b['last_seen']    = $r['received_at'];
            $b['clock_synced'] = !empty($r['clock_synced']);
        }
        unset($b);

        $recent[] = [
            'received_at' => $r['received_at'],
            'box_id' => $box, 'service_id' => $sid, 'name' => $name,
            'path' => $path, 'sat_source' => $sat,
            'duration_ms' => $dur, 'duration' => fmt_hms($dur),
            'first_frame_ms' => $ff, 'failed' => $failed,
        ];
    }

    $summary['boxes']    = count($boxes);
    $summary['watch']    = fmt_hms($summary['watch_ms']);

    foreach ($channels as &$c) {
        $c['watch']  = fmt_hms($c['watch_ms']);
        $c['avg']    = $c['views'] ? fmt_hms((int)($c['watch_ms'] / $c['views'])) : '-';
        $c['avg_ff'] = $c['ff_n'] ? (int)round($c['ff_sum'] / $c['ff_n']) : -1;
        unset($c['ff_sum'], $c['ff_n']);
    }
    unset($c);
    foreach ($boxes as &$b) { $b['watch'] = fmt_hms($b['watch_ms']); }
    unset($b);

    usort($channels, function ($x, $y) { return $y['watch_ms'] <=> $x['watch_ms']; });
    usort($boxes,    function ($x, $y) { return $y['watch_ms'] <=> $x['watch_ms']; });
    $recent = array_slice(array_reverse($recent), 0, 60);

    emit([
        'range'       => $range,
        'server_time' => date('c'),
        'log_present' => is_readable(STATS_LOG),
        'summary'     => $summary,
        'channels'    => array_values($channels),
        'boxes'       => array_values($boxes),
        'recent'      => $recent,
    ]);

} catch (Exception $e) {
    logMessage('ERROR', 'Viewing API error: ' . $e->getMessage());
    emit(['error' => true, 'message' => 'Internal error'], 500);
}
