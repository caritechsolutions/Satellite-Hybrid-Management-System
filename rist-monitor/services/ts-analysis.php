<?php
// services/ts-analysis.php - one TSDuck analysis implementation, shared.
//
// Extracted verbatim from ChannelService so the Part 7 and Part 8 channel types
// cannot drift on the shape of a TSDuck report. There is exactly one place that
// probes TSDuck's key spellings, and one place that decides what a PID belongs
// to; two copies of that would be two chances for the headend and the box to
// disagree about which PIDs a service owns.

if (!defined('TSP_BINARY'))      define('TSP_BINARY', '/usr/bin/tsp');
if (!defined('ANALYSE_SECONDS')) define('ANALYSE_SECONDS', 5);
if (!defined('STATS_LOCK_DIR'))  define('STATS_LOCK_DIR', sys_get_temp_dir());

/**
 * Run tsp against a UDP source and return the parsed report.
 *
 * @param string $inputUrl  udp://@host:port (or bare host:port)
 * @param string $lockKey   one analysis at a time per key
 */
function ts_analyse_udp($inputUrl, $lockKey)
{
    if (!is_executable(TSP_BINARY)) throw new Exception('TSDuck (tsp) is not installed');

    $safeKey = preg_replace('/[^a-z0-9_-]/i', '_', (string)$lockKey);
    $lock = STATS_LOCK_DIR . "/analyse-{$safeKey}.lock";
    $fp = @fopen($lock, 'c');
    if (!$fp || !flock($fp, LOCK_EX | LOCK_NB)) {
        if ($fp) fclose($fp);
        throw new Exception('An analysis is already running for this channel');
    }

    try {
        $out = sys_get_temp_dir() . "/tsp-analyse-{$safeKey}.json";
        @unlink($out);

        // -I ip wants host:port with no scheme
        $src = preg_replace('#^udp://@?#', '', $inputUrl);

        // timeout is a hard backstop: tsp on a dead input waits forever
        $cmd = sprintf(
            'timeout %d %s -I ip %s -P until --seconds %d '
          . '-P analyze --json --output-file %s -O drop 2>&1',
            ANALYSE_SECONDS + 10,
            escapeshellcmd(TSP_BINARY),
            escapeshellarg($src),
            ANALYSE_SECONDS,
            escapeshellarg($out)
        );
        exec($cmd, $lines, $rc);

        if (!is_file($out) || filesize($out) === 0) {
            throw new Exception('No data from ' . $inputUrl
                . ' - is the stream running? (' . trim(implode(' ', array_slice($lines, -3))) . ')');
        }

        $raw = json_decode(file_get_contents($out), true);
        @unlink($out);
        if (!is_array($raw)) throw new Exception('Could not parse the TSDuck report');

        $parsed = ts_parse_analysis($raw);
        $parsed['analysed_at'] = date('c');
        return $parsed;

    } finally {
        flock($fp, LOCK_UN);
        fclose($fp);
    }
}

// TSDuck's JSON key names vary a little by version, so probe a few spellings
// rather than assuming one. Unknown shapes degrade to an empty list instead
// of throwing - the raw report is kept so we can tighten this later.
function ts_parse_analysis($raw)
{
    $pick = function ($arr, $keys, $default = null) {
        foreach ($keys as $k) {
            if (is_array($arr) && array_key_exists($k, $arr) && $arr[$k] !== null) return $arr[$k];
        }
        return $default;
    };

    $svcList = $pick($raw, ['services'], []);
    $svc     = is_array($svcList) && count($svcList) ? reset($svcList) : [];

    $out = [
        'service_id' => (int)$pick($svc, ['id', 'service-id', 'service_id'], 0),
        'name'       => (string)$pick($svc, ['name', 'service-name'], ''),
        'provider'   => (string)$pick($svc, ['provider', 'service-provider'], ''),
        'pmt_pid'    => (int)$pick($svc, ['pmt-pid', 'pmt_pid'], 0),
        'pcr_pid'    => (int)$pick($svc, ['pcr-pid', 'pcr_pid'], 0),
        'ts_id'      => (int)$pick($pick($raw, ['ts'], []), ['id', 'ts-id'], 0),
        'services'   => [],
        'streams'    => [],
    ];

    // Every service, WITH ITS OWN pmt_pid and pcr_pid.
    //
    // The top-level service_id / pmt_pid / pcr_pid above describe services[0]
    // and nothing else. On a single-service input that is the whole report; on
    // an MPTS transponder ingest it is whichever service TSDuck listed first,
    // which is almost never the one a Part 8 channel selected.
    //
    // That is not cosmetic, and it shipped broken. On the live 5800 feed the
    // first service is TLC-GUYANA (PMT 0x008D, PCR 0x0021) while NCN-Guyana is
    // service 2 with PMT 0x008E and PCR 0x0022. Taking the top-level values
    // gave the cutter -C 33 -- a PID it can never see a PCR on, so it never
    // cuts -- and put TLC's PMT into NCN's filter set, which makes the two
    // ends' byte streams differ without saying anything.
    foreach ((is_array($svcList) ? $svcList : []) as $s) {
        if (!is_array($s)) continue;
        $out['services'][] = [
            'id'      => (int)$pick($s, ['id', 'service-id', 'service_id'], 0),
            'name'    => (string)$pick($s, ['name', 'service-name'], ''),
            'pmt_pid' => (int)$pick($s, ['pmt-pid', 'pmt_pid'], 0),
            'pcr_pid' => (int)$pick($s, ['pcr-pid', 'pcr_pid'], 0),
        ];
    }

    foreach ($pick($raw, ['pids'], []) as $p) {
        if (!is_array($p)) continue;
        $pid = (int)$pick($p, ['id', 'pid'], -1);
        if ($pid < 0) continue;

        // Skip stuffing and the PSI we never remap
        if ($pid === 0x1FFF || $pid === 0x0000 || $pid === 0x0011 ||
            $pid === 0x0010 || $pid === 0x0014 || $pid === 0x0001) continue;

        // Which service(s) own this PID. TSDuck spells this several ways
        // depending on version, and some reports omit it entirely - so it is
        // probed, and its ABSENCE is recorded as null rather than as an
        // empty list. p8FilterPids() treats those differently: on a
        // single-service report absent means "this service", on an MPTS
        // report it means "unknown", and guessing there would silently drop
        // audio.
        $owners = null;
        foreach (['services', 'service-list', 'service_ids'] as $k) {
            if (isset($p[$k]) && is_array($p[$k])) {
                $owners = array_values(array_map('intval', $p[$k]));
                break;
            }
        }
        if ($owners === null) {
            foreach (['service-id', 'service_id'] as $k) {
                if (isset($p[$k]) && is_numeric($p[$k])) { $owners = [(int)$p[$k]]; break; }
            }
        }

        $stream = [
            'pid'         => $pid,
            'description' => (string)$pick($p, ['description', 'usage'], ''),
            'bitrate'     => (int)$pick($p, ['bitrate'], 0),
            'is_pmt'      => ($pid === $out['pmt_pid']),
            'is_pcr'      => ($pid === $out['pcr_pid']),
        ];
        if ($owners !== null) $stream['services'] = $owners;
        $out['streams'][] = $stream;
    }
    usort($out['streams'], function ($a, $b) { return $a['pid'] <=> $b['pid']; });
    return $out;
}
