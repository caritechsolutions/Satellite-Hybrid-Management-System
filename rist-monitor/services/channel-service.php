<?php
// services/channel-service.php - Channel management for the Part 7 headend sender
//
// One channel produces two systemd units:
//   ristsender-<id>.service  - ristsender with a weight-0 and a weight-1000 peer
//   ristmarker-<id>.service  - ristreceiver_with_markers, bound to the sender,
//                              converting the weight-0 peer back to TS with
//                              Part 7 markers for the uplink mux.

require_once dirname(__DIR__) . '/config/config.php';

if (!defined('CHANNELS_FILE'))      define('CHANNELS_FILE', CONFIG_DIR . '/channels.json');
if (!defined('MARKER_BINARY'))      define('MARKER_BINARY', '/usr/local/bin/ristreceiver_with_markers');
if (!defined('SYSTEMD_DIR'))        define('SYSTEMD_DIR', '/etc/systemd/system');
if (!defined('UNIT_HELPER'))        define('UNIT_HELPER', '/usr/local/sbin/rist-unit');
if (!defined('PORT_BASE_SAT'))      define('PORT_BASE_SAT', 5600);
if (!defined('PORT_BASE_RECOVERY')) define('PORT_BASE_RECOVERY', 5700);
if (!defined('PORT_BASE_METRICS'))  define('PORT_BASE_METRICS', 6000);
if (!defined('DEFAULT_MARKER_PID')) define('DEFAULT_MARKER_PID', 0x1FF0); // 8176

class ChannelService
{
    private $file;

    public function __construct()
    {
        $this->file = CHANNELS_FILE;
        if (!file_exists($this->file)) {
            $this->write(['settings' => ['server_ip' => $this->detectServerIp()], 'channels' => []]);
        }
    }

    // ---------------------------------------------------------------- store

    private function read()
    {
        $raw = @file_get_contents($this->file);
        $data = $raw ? json_decode($raw, true) : null;
        if (!is_array($data)) $data = [];
        if (!isset($data['channels']) || !is_array($data['channels'])) $data['channels'] = [];
        if (!isset($data['settings']) || !is_array($data['settings'])) $data['settings'] = [];
        if (empty($data['settings']['server_ip'])) $data['settings']['server_ip'] = $this->detectServerIp();
        return $data;
    }

    private function write($data)
    {
        $json = json_encode($data, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES);
        if (file_put_contents($this->file, $json, LOCK_EX) === false) {
            throw new Exception('Cannot write ' . $this->file . ' (check permissions)');
        }
        return true;
    }

    public function detectServerIp()
    {
        $out = trim((string)@shell_exec("hostname -I 2>/dev/null | awk '{print \$1}'"));
        if ($out !== '' && filter_var($out, FILTER_VALIDATE_IP)) return $out;
        return $_SERVER['SERVER_ADDR'] ?? '127.0.0.1';
    }

    public function getSettings()
    {
        return $this->read()['settings'];
    }

    public function saveSettings($settings)
    {
        $data = $this->read();
        if (!empty($settings['server_ip'])) {
            if (!filter_var($settings['server_ip'], FILTER_VALIDATE_IP)) {
                throw new Exception('server_ip is not a valid IP address');
            }
            $data['settings']['server_ip'] = $settings['server_ip'];
        }
        if (array_key_exists('recovery_ip', $settings)) {
            $rip = trim((string)$settings['recovery_ip']);
            if ($rip === '') {
                unset($data['settings']['recovery_ip']);          // fall back to server_ip
            } elseif (filter_var($rip, FILTER_VALIDATE_IP)
                   || preg_match('/^[a-zA-Z0-9][a-zA-Z0-9\.\-]{0,253}$/', $rip)) {
                $data['settings']['recovery_ip'] = $rip;
            } else {
                throw new Exception('Recovery address must be an IP or hostname');
            }
        }
        if (isset($settings['buffer']) && is_numeric($settings['buffer'])) {
            $data['settings']['buffer'] = (int)$settings['buffer'];
        }
        $this->write($data);
        return $data['settings'];
    }

    // ---------------------------------------------------------------- read

    public function getChannels()
    {
        $data = $this->read();
        foreach ($data['channels'] as &$ch) {
            $ch['status'] = $this->serviceState('ristsender-' . $ch['id']);
            $ch['marker_status'] = $this->serviceState('ristmarker-' . $ch['id']);
        }
        return $data['channels'];
    }

    public function getChannel($id)
    {
        foreach ($this->read()['channels'] as $ch) {
            if ($ch['id'] === $id) {
                $ch['status'] = $this->serviceState('ristsender-' . $id);
                $ch['marker_status'] = $this->serviceState('ristmarker-' . $id);
                return $ch;
            }
        }
        return null;
    }

    // ---------------------------------------------------------------- write

    public function createChannel($input)
    {
        $data = $this->read();

        $name = trim((string)($input['name'] ?? ''));
        if ($name === '') throw new Exception('Channel name is required');

        $id = $this->slug($name);
        foreach ($data['channels'] as $ch) {
            if ($ch['id'] === $id) throw new Exception("A channel named '{$name}' already exists");
        }

        $serviceId = $this->validId($input['service_id'] ?? 0, 'Service ID', true);
        foreach ($data['channels'] as $ch) {
            if ((int)($ch['service_id'] ?? 0) === $serviceId) {
                throw new Exception("Service ID {$serviceId} is already used by channel '{$ch['name']}'");
            }
        }

        $channel = [
            'id'             => $id,
            'name'           => $name,
            'service_id'     => $serviceId,
            'ts_id'          => $this->validId($input['ts_id'] ?? 0, 'Transport stream ID', false),
            'input_url'      => $this->normaliseUdp($input['input_url'] ?? '', true),
            'uplink_url'     => $this->normaliseUdp($input['uplink_url'] ?? '', false),
            'marker_pid'     => $this->validPid($input['marker_pid'] ?? DEFAULT_MARKER_PID),
            'buffer'         => $this->validBuffer($input['buffer'] ?? DEFAULT_BUFFER_SIZE),
            'enabled'        => true,
            'created_at'     => date('c'),
        ];

        // Ports are allocated automatically and shown read-only in the UI
        $ports = $this->allocatePorts($data['channels']);
        $channel = array_merge($channel, $ports);

        $data['channels'][] = $channel;
        $this->write($data);

        $this->writeUnits($channel, $data['settings']);
        logMessage('INFO', "Channel created: {$id}", $channel);

        return $channel;
    }

    public function updateChannel($id, $input)
    {
        $data = $this->read();
        $found = false;

        foreach ($data['channels'] as &$ch) {
            if ($ch['id'] !== $id) continue;
            $found = true;

            if (isset($input['name']) && trim($input['name']) !== '') $ch['name'] = trim($input['name']);
            if (isset($input['service_id'])) {
                $sid = $this->validId($input['service_id'], 'Service ID', true);
                foreach ($data['channels'] as $other) {
                    if ($other['id'] !== $id && (int)($other['service_id'] ?? 0) === $sid) {
                        throw new Exception("Service ID {$sid} is already used by channel '{$other['name']}'");
                    }
                }
                $ch['service_id'] = $sid;
            }
            if (isset($input['ts_id'])) $ch['ts_id'] = $this->validId($input['ts_id'], 'Transport stream ID', false);
            if (isset($input['input_url']))  $ch['input_url']  = $this->normaliseUdp($input['input_url'], true);
            if (isset($input['uplink_url'])) $ch['uplink_url'] = $this->normaliseUdp($input['uplink_url'], false);
            if (isset($input['marker_pid'])) $ch['marker_pid'] = $this->validPid($input['marker_pid']);
            if (isset($input['buffer'])) $ch['buffer'] = $this->validBuffer($input['buffer']);
            $ch['updated_at'] = date('c');

            $updated = $ch;
        }
        unset($ch);

        if (!$found) throw new Exception("Channel '{$id}' not found");

        $this->write($data);
        $this->writeUnits($updated, $data['settings']);

        // Reload the running services so changes take effect
        if ($this->serviceState('ristsender-' . $id) === 'active') {
            $this->restartChannel($id);
        }

        logMessage('INFO', "Channel updated: {$id}");
        return $updated;
    }

    public function deleteChannel($id)
    {
        $data = $this->read();
        $before = count($data['channels']);
        $data['channels'] = array_values(array_filter(
            $data['channels'],
            function ($ch) use ($id) { return $ch['id'] !== $id; }
        ));
        if (count($data['channels']) === $before) throw new Exception("Channel '{$id}' not found");

        $this->stopChannel($id);
        $this->removeUnits($id);
        $this->write($data);

        logMessage('INFO', "Channel deleted: {$id}");
        return true;
    }

    // ---------------------------------------------------------------- ports

    private function allocatePorts($existing)
    {
        $usedSat = $usedRec = $usedMet = [];
        foreach ($existing as $ch) {
            if (isset($ch['sat_port']))      $usedSat[] = (int)$ch['sat_port'];
            if (isset($ch['recovery_port'])) $usedRec[] = (int)$ch['recovery_port'];
            if (isset($ch['metrics_port']))  $usedMet[] = (int)$ch['metrics_port'];
        }
        return [
            'sat_port'      => $this->nextFree(PORT_BASE_SAT, $usedSat),
            'recovery_port' => $this->nextFree(PORT_BASE_RECOVERY, $usedRec),
            'metrics_port'  => $this->nextFree(PORT_BASE_METRICS, $usedMet),
        ];
    }

    private function nextFree($base, $used)
    {
        for ($p = $base; $p < $base + 100; $p++) {
            if (!in_array($p, $used, true)) return $p;
        }
        throw new Exception("No free port in the {$base} range");
    }

    // ---------------------------------------------------------------- systemd

    public function buildSenderCommand($ch, $settings)
    {
        $ip     = $settings['server_ip'];
        $buffer = (int)($ch['buffer'] ?? DEFAULT_BUFFER_SIZE);

        $peers = sprintf(
            'rist://@%s:%d?weight=%d&buffer=%d,rist://@%s:%d?weight=%d&buffer=%d',
            $ip, $ch['sat_port'],      DEFAULT_WEIGHT_SATELLITE, $buffer,
            $ip, $ch['recovery_port'], DEFAULT_WEIGHT_RECOVERY,  $buffer
        );

        // NOTE: -n (NPD) is deliberately NOT passed - it conflicts with Part 6
        // program selection, which applies NPD automatically after filtering.
        return sprintf(
            '%s -i %s -o "%s" --metrics-http --metrics-port %d --verbose-level 1',
            RIST_SENDER_BINARY,
            $ch['input_url'],
            $peers,
            $ch['metrics_port']
        );
    }

    public function buildMarkerCommand($ch, $settings)
    {
        // Connects to the sender's weight-0 peer, emits marked TS to the uplink.
        // The -P marker-pid switch lands when the tool is updated; stored now.
        return sprintf(
            '%s -i rist://%s:%d -o %s',
            MARKER_BINARY,
            $settings['server_ip'],
            $ch['sat_port'],
            $ch['uplink_url']
        );
    }

    private function writeUnits($ch, $settings)
    {
        $id     = $ch['id'];
        $sender = 'ristsender-' . $id;
        $marker = 'ristmarker-' . $id;

        $senderUnit = "[Unit]\n"
            . "Description=RIST Sender - {$ch['name']}\n"
            . "After=network-online.target\n"
            . "Wants=network-online.target\n\n"
            . "[Service]\n"
            . "Type=simple\n"
            . "ExecStart=" . $this->buildSenderCommand($ch, $settings) . "\n"
            . "Restart=always\n"
            . "RestartSec=3\n"
            . "StandardOutput=journal\n"
            . "StandardError=journal\n\n"
            . "[Install]\n"
            . "WantedBy=multi-user.target\n";

        // BindsTo + PartOf + WantedBy means the marker follows the sender:
        // starting/stopping/restarting the sender does the same to the marker.
        $markerUnit = "[Unit]\n"
            . "Description=RIST Marker Receiver - {$ch['name']}\n"
            . "After={$sender}.service\n"
            . "BindsTo={$sender}.service\n"
            . "PartOf={$sender}.service\n\n"
            . "[Service]\n"
            . "Type=simple\n"
            . "ExecStart=" . $this->buildMarkerCommand($ch, $settings) . "\n"
            . "Restart=always\n"
            . "RestartSec=3\n"
            . "StandardOutput=journal\n"
            . "StandardError=journal\n\n"
            . "[Install]\n"
            . "WantedBy={$sender}.service\n";

        $this->putUnit($sender, $senderUnit);
        $this->putUnit($marker, $markerUnit);

        $this->systemctl('daemon-reload');
        $this->systemctl('enable', $marker);   // so it starts with the sender
    }

    private function putUnit($unitName, $contents)
    {
        // www-data cannot write /etc/systemd/system, so a narrow root helper
        // (/usr/local/sbin/rist-unit) does it - it only accepts ristsender-*
        // and ristmarker-* names.
        $tmp = '/tmp/' . $unitName . '.' . getmypid();
        file_put_contents($tmp, $contents);
        exec('sudo ' . UNIT_HELPER . ' install ' . escapeshellarg($unitName) . ' '
             . escapeshellarg($tmp) . ' 2>&1', $o, $rc);
        @unlink($tmp);
        if ($rc !== 0) {
            throw new Exception("Cannot install unit {$unitName}: " . implode(' ', $o));
        }
    }

    private function removeUnits($id)
    {
        foreach (["ristmarker-{$id}", "ristsender-{$id}"] as $svc) {
            $this->systemctl('disable', $svc);
            exec('sudo ' . UNIT_HELPER . ' remove ' . escapeshellarg($svc) . ' 2>&1');
        }
        $this->systemctl('daemon-reload');
    }

    // ---------------------------------------------------------------- control

    public function startChannel($id)   { return $this->systemctl('start',   'ristsender-' . $id); }
    public function stopChannel($id)    { return $this->systemctl('stop',    'ristsender-' . $id); }
    public function restartChannel($id) { return $this->systemctl('restart', 'ristsender-' . $id); }

    private function systemctl($action, $unit = null)
    {
        $cmd = 'sudo ' . SYSTEMCTL_BINARY . ' ' . escapeshellarg($action);
        if ($unit !== null) $cmd .= ' ' . escapeshellarg($unit);
        $cmd .= ' 2>&1';
        exec($cmd, $out, $rc);
        if ($rc !== 0 && $action !== 'disable') {
            logMessage('WARNING', "systemctl {$action} {$unit} failed", ['out' => $out]);
        }
        return $rc === 0;
    }

    private function serviceState($unit)
    {
        exec('sudo ' . SYSTEMCTL_BINARY . ' is-active ' . escapeshellarg($unit) . ' 2>&1', $out, $rc);
        $state = trim(implode('', $out));
        return $state !== '' ? $state : 'unknown';
    }

    // ---------------------------------------------------------------- recovery

    // The list the set-top boxes fetch at boot. Only channels whose sender is
    // actually running are advertised - a box should never be pointed at a
    // recovery peer that is not listening.
    public function getRecoveryChannels()
    {
        $data = $this->read();
        // What the boxes are TOLD to connect to. Defaults to this server's own
        // IP, but can be overridden (different subnet, NAT, or a DNS name).
        $ip   = !empty($data['settings']['recovery_ip'])
              ? $data['settings']['recovery_ip']
              : $data['settings']['server_ip'];
        $out  = [];

        foreach ($data['channels'] as $ch) {
            if ((int)($ch['service_id'] ?? 0) === 0) continue;                 // not addressable
            if ($this->serviceState('ristsender-' . $ch['id']) !== 'active') continue;

            $out[] = [
                'service_id' => (int)$ch['service_id'],
                'ts_id'      => (int)($ch['ts_id'] ?? 0),
                'name'       => $ch['name'],
                'marker_pid' => (int)$ch['marker_pid'],
                'rist_url'   => sprintf('rist://%s:%d?buffer=%d',
                                        $ip,
                                        (int)$ch['recovery_port'],
                                        (int)($ch['buffer'] ?? DEFAULT_BUFFER_SIZE)),
            ];
        }
        return $out;
    }

    // ---------------------------------------------------------------- stats

    // Reads the channel's librist Prometheus exporter and groups the samples
    // per peer. The "listening" label tells us which peer a sample belongs to:
    // the sat_port peer is the satellite path (weight 0), the recovery_port
    // peer(s) are the receivers pulling recovery (weight 1000).
    public function getChannelStats($id)
    {
        $ch = $this->getChannel($id);
        if (!$ch) throw new Exception("Channel '{$id}' not found");

        $out = [
            'channel_id'   => $id,
            'metrics_port' => (int)$ch['metrics_port'],
            'available'    => false,
            'satellite'    => [],
            'recovery'     => [],
            'totals'       => ['peers' => 0, 'bandwidth_bps' => 0, 'retransmitted' => 0],
        ];

        if ($this->serviceState('ristsender-' . $id) !== 'active') return $out;

        $ctx = stream_context_create(['http' => ['timeout' => 2, 'ignore_errors' => true]]);
        $raw = @file_get_contents('http://127.0.0.1:' . (int)$ch['metrics_port'] . '/metrics', false, $ctx);
        if ($raw === false || trim($raw) === '') return $out;
        $out['available'] = true;

        foreach ($this->parsePrometheus($raw) as $peer) {
            $listening = $peer['listening'] ?? ':0';
            $port = (int)substr(strrchr($listening, ':'), 1);

            $entry = [
                'peer_id'       => $peer['peer_id'] ?? '?',
                'remote'        => $peer['peer_url'] ?? '-',
                'cname'         => $peer['cname'] ?? '-',
                'bandwidth_bps' => (float)($peer['rist_sender_peer_bandwidth_bps'] ?? 0),
                'retry_bps'     => (float)($peer['rist_sender_peer_retry_bandwidth_bps'] ?? 0),
                'sent'          => (int)($peer['rist_sender_peer_sent_packets'] ?? 0),
                'retransmitted' => (int)($peer['rist_sender_peer_retransmitted_packets'] ?? 0),
                'received'      => (int)($peer['rist_sender_peer_received_packets'] ?? 0),
                'rtt_ms'        => round(((float)($peer['rist_sender_peer_rtt_seconds'] ?? 0)) * 1000, 1),
                'quality'       => (float)($peer['rist_sender_peer_quality'] ?? 0),
            ];

            if ($port === (int)$ch['recovery_port']) {
                $out['recovery'][] = $entry;
            } else {
                $out['satellite'][] = $entry;
            }
            $out['totals']['peers']++;
            $out['totals']['bandwidth_bps'] += $entry['bandwidth_bps'];
            $out['totals']['retransmitted'] += $entry['retransmitted'];
        }

        return $out;
    }

    private function parsePrometheus($raw)
    {
        $peers = [];
        foreach (explode("\n", $raw) as $line) {
            $line = trim($line);
            if ($line === '' || $line[0] === '#') continue;
            if (!preg_match('/^([a-zA-Z0-9_]+)\{([^}]*)\}\s+([-0-9.eE+]+)$/', $line, $m)) continue;

            $labels = [];
            if (preg_match_all('/([a-zA-Z0-9_]+)="([^"]*)"/', $m[2], $lm, PREG_SET_ORDER)) {
                foreach ($lm as $l) $labels[$l[1]] = $l[2];
            }
            $key = ($labels['sender_id'] ?? '0') . ':' . ($labels['peer_id'] ?? '?');
            if (!isset($peers[$key])) $peers[$key] = $labels;
            $peers[$key][$m[1]] = $m[3];
        }
        return array_values($peers);
    }

    // ---------------------------------------------------------------- helpers

    private function slug($name)
    {
        $s = strtolower(trim($name));
        $s = preg_replace('/[^a-z0-9]+/', '-', $s);
        $s = trim($s, '-');
        return $s !== '' ? $s : 'channel-' . substr(md5($name), 0, 6);
    }

    private function normaliseUdp($url, $listen)
    {
        $url = trim((string)$url);
        if ($url === '') throw new Exception('UDP address is required');

        if (strpos($url, 'udp://') !== 0) $url = 'udp://' . $url;

        // Input listens (udp://@host:port); uplink output does not use '@'
        $body = substr($url, 6);
        $body = ltrim($body, '@');
        if (!preg_match('/^[0-9a-zA-Z\.\-]+:\d{1,5}$/', $body)) {
            throw new Exception("Invalid UDP address '{$url}' - expected udp://host:port");
        }
        return 'udp://' . ($listen ? '@' : '') . $body;
    }

    // The RIST retransmission window, in ms. It is also the live delay, so a
    // typo here costs either recovery headroom or seconds of latency.
    private function validBuffer($v)
    {
        $v = (int)$v;
        if ($v < 100 || $v > 60000) {
            throw new Exception('RIST buffer must be between 100 and 60000 ms');
        }
        return $v;
    }

    private function validId($v, $label, $required)
    {
        if (is_string($v) && stripos($v, '0x') === 0) $v = hexdec(substr($v, 2));
        $v = (int)$v;
        if ($v === 0) {
            if ($required) throw new Exception("{$label} is required");
            return 0;
        }
        if ($v < 1 || $v > 65535) throw new Exception("{$label} must be between 1 and 65535");
        return $v;
    }

    private function validPid($pid)
    {
        if (is_string($pid) && stripos($pid, '0x') === 0) $pid = hexdec(substr($pid, 2));
        $pid = (int)$pid;
        if ($pid < 0x0020 || $pid > 0x1FFE) {
            throw new Exception('Marker PID must be between 32 (0x0020) and 8190 (0x1FFE)');
        }
        return $pid;
    }
}
