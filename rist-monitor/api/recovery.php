<?php
// api/recovery.php - Set-top box facing recovery lookup + viewing-stats ingest
//
//   GET  /api/recovery.php                 -> channel list
//   GET  /api/recovery.php?service_id=N    -> single channel, 404 if none
//   POST /api/recovery.php                 -> ingest stats body, return channel list + ack_id
//
// The POST body is the box's viewing-stats batch (schema 1). We store what we
// can, then reply with the recovery list AND an ack_id = the highest record id
// durably written for that box. The box drops id <= ack_id and retries the rest.
// Omitting ack_id (or 0) means "kept nothing" - the box will resend.

require_once dirname(__DIR__) . '/config/config.php';
require_once dirname(__DIR__) . '/services/channel-service.php';
require_once dirname(__DIR__) . '/services/part8-service.php';

if (!defined('STATS_DIR'))  define('STATS_DIR',  DATA_DIR . '/stats');
if (!defined('STATS_LOG'))  define('STATS_LOG',  STATS_DIR . '/views.jsonl');
if (!defined('STATS_ACKS')) define('STATS_ACKS', STATS_DIR . '/acks.json');
if (!defined('STATS_RAW'))  define('STATS_RAW',  STATS_DIR . '/last-raw.json');

// Emit locally so we can drop the escaped slashes - boxes parse this payload.
function emit($payload, $status = 200)
{
    http_response_code($status);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($payload, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES);
    exit;
}

// ---------------------------------------------------------------- stats

function stats_dir_ready()
{
    if (!is_dir(STATS_DIR)) @mkdir(STATS_DIR, 0775, true);
    return is_dir(STATS_DIR) && is_writable(STATS_DIR);
}

function stats_load_acks()
{
    $raw = @file_get_contents(STATS_ACKS);
    $a = $raw ? json_decode($raw, true) : null;
    return is_array($a) ? $a : [];
}

function stats_save_acks($acks)
{
    @file_put_contents(STATS_ACKS, json_encode($acks, JSON_PRETTY_PRINT), LOCK_EX);
}

/**
 * Store a batch. Returns the highest record id durably written for this box,
 * or 0 if nothing was stored (box will resend).
 */
function stats_ingest($body, $raw)
{
    if (!stats_dir_ready()) {
        logMessage('ERROR', 'stats: ' . STATS_DIR . ' not writable - not acking');
        return 0;
    }

    // Keep the most recent raw body for schema inspection during bring-up.
    @file_put_contents(STATS_RAW, $raw);

    $boxId = isset($body['box_id']) ? (string)$body['box_id'] : '';
    if ($boxId === '') { logMessage('WARNING', 'stats: no box_id'); return 0; }

    $records = isset($body['records']) && is_array($body['records']) ? $body['records'] : [];
    if (!$records) return 0;   // nothing to ack

    $acks   = stats_load_acks();
    $lastId = isset($acks[$boxId]) ? (int)$acks[$boxId] : 0;

    // Untrusted unless the box says its clock was synced this boot.
    $clockOk = !empty($body['clock_synced']);
    $boxTime = isset($body['box_time']) ? (int)$body['box_time'] : 0;

    $lines = '';
    $maxId = $lastId;
    $kept  = 0;

    foreach ($records as $r) {
        $id = isset($r['id']) ? (int)$r['id'] : 0;
        if ($id <= 0)      continue;   // malformed
        if ($id <= $lastId) { $maxId = max($maxId, $id); continue; }  // already have it

        $row = [
            'box_id'          => $boxId,
            'id'              => $id,
            'service_id'      => isset($r['service_id']) ? (int)$r['service_id'] : 0,
            'ts_id'           => isset($r['ts_id']) ? (int)$r['ts_id'] : 0,
            'name'            => isset($r['name']) ? (string)$r['name'] : '',
            'path'            => isset($r['path']) ? (string)$r['path'] : '',
            'sat_source'      => isset($r['sat_source']) ? (string)$r['sat_source'] : '',
            'start_uptime_ms' => isset($r['start_uptime_ms']) ? (int)$r['start_uptime_ms'] : 0,
            'duration_ms'     => isset($r['duration_ms']) ? (int)$r['duration_ms'] : 0,
            'first_frame_ms'  => isset($r['first_frame_ms']) ? (int)$r['first_frame_ms'] : -1,
            // Server-applied wall clock - authoritative, unlike box_time.
            'received_at'     => date('c'),
            'box_time'        => $boxTime,
            'clock_synced'    => $clockOk,
        ];
        $lines .= json_encode($row, JSON_UNESCAPED_SLASHES) . "\n";
        $maxId  = max($maxId, $id);
        $kept++;
    }

    if ($lines !== '') {
        if (@file_put_contents(STATS_LOG, $lines, FILE_APPEND | LOCK_EX) === false) {
            logMessage('ERROR', 'stats: cannot append ' . STATS_LOG . ' - not acking');
            return 0;   // durability failed: do NOT ack, box retries
        }
    }

    if ($maxId > $lastId) {
        $acks[$boxId] = $maxId;
        stats_save_acks($acks);
    }

    $dropped = isset($body['dropped']) ? (int)$body['dropped'] : 0;
    logMessage('INFO', "stats: box={$boxId} kept={$kept} ack={$maxId}"
                     . ($dropped ? " box_dropped={$dropped}" : ''));

    return $maxId;
}

// ---------------------------------------------------------------- request

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') { setCORSHeaders(); exit; }
setCORSHeaders();

$method = $_SERVER['REQUEST_METHOD'];
if ($method !== 'GET' && $method !== 'POST') {
    emit(['error' => true, 'message' => 'Method not allowed'], 405);
}

try {
    $svc      = new ChannelService();
    $p8       = new Part8Service();

    // Two independent lists. Part 7 channels and Part 8 channels are separate
    // records that do not reference each other; the API advertises whatever
    // exists. A service with both gets ONE record carrying both sets of keys,
    // so the box still does a single lookup: a Part 7 box finds rist_url and
    // ignores what it does not know, a Part 8 box finds part8_rist_url.
    $channels = $svc->getRecoveryChannels();
    $byService = [];
    foreach ($channels as $i => $c) $byService[(int)$c['service_id']] = $i;

    foreach ($p8->getRecoveryChannels() as $rec) {
        $sid = (int)$rec['service_id'];
        if (isset($byService[$sid])) {
            // Merge INTO the Part 7 record, never over it: name and ts_id are
            // whatever Part 7 already advertised, because that is what boxes in
            // the field have been seeing.
            foreach ($rec as $k => $v) {
                if ($k === 'name' || $k === 'ts_id' || $k === 'service_id') continue;
                $channels[$byService[$sid]][$k] = $v;
            }
        } else {
            $byService[$sid] = count($channels);
            $channels[] = $rec;
        }
    }

    // Single-service lookup (GET only) - 404 tells the box to stay on the
    // normal decode path for that service.
    if ($method === 'GET' && isset($_GET['service_id'])) {
        $want = (int)$_GET['service_id'];
        foreach ($channels as $ch) {
            if ($ch['service_id'] !== $want) continue;

            // THE ANCHOR (B3). ?pcr=<33-bit PCR base> asks which RTP sequence
            // carries it, answered from the sender's own PCR -> sequence
            // catalogue -- the same index the retransmit path serves from, so
            // it cannot disagree with what a NACK would return.
            //
            // It rides the GET the box already makes at zap: no second
            // connection, no new protocol, and no query load on the sender's
            // protocol thread, which services every peer's event loop while
            // holding peerlist_lock.
            if (isset($_GET['pcr']) && !empty($ch['part8'])) {
                $p8id = $p8->channelIdForService($want);
                if ($p8id !== null) {
                    try {
                        $ch['anchor'] = $p8->anchor(
                            $p8id, $_GET['pcr'], (int)($_GET['duration_ms'] ?? 0));
                    } catch (Exception $e) {
                        // A failed anchor must not cost the box its record.
                        $ch['anchor'] = ['status' => 'UNAVAILABLE',
                                         'note'   => $e->getMessage()];
                    }
                }
            }
            emit($ch);
        }
        emit(['error' => true,
              'message' => "No RIST recovery for service_id {$want}"], 404);
    }

    $ackId = 0;
    if ($method === 'POST') {
        $raw  = file_get_contents('php://input');
        $body = json_decode($raw, true);
        if (is_array($body)) {
            $ackId = stats_ingest($body, $raw);
        } else {
            // Bad body must NOT cost the box its channel list.
            logMessage('WARNING', 'stats: unparseable POST body (' . strlen($raw) . ' bytes)');
        }
    }

    $out = [
        'server_time' => date('c'),
        'count'       => count($channels),
        'channels'    => $channels,
    ];
    if ($method === 'POST') $out['ack_id'] = $ackId;

    emit($out);

} catch (Exception $e) {
    logMessage('ERROR', 'Recovery API error: ' . $e->getMessage());
    emit(['error' => true, 'message' => 'Internal error'], 500);
}
