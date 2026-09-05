<?php
// services/part8-service.php - Part 8 recovery channels.
//
// A STANDALONE CHANNEL TYPE, not a mode of a Part 7 channel. A Part 8 channel
// has no marker, no uplink output, no satellite peer and no mux chain, because
// the services it recovers are not ones we mux -- they arrive on the
// transponder and go out on it unchanged. Making Part 8 a flag on the Part 7
// record meant configuring a Part 7 channel nobody wanted in order to reach one
// checkbox.
//
// The whole chain, per channel:
//
//   shared transponder ingest (multicast, existing, untouched)
//     -> tsp -r -I ip <ingest> -P filter --pid <this service's PIDs>
//                              -O ip 127.0.0.1:<port>
//     -> part8_recovery_server -i udp://@127.0.0.1:<port>
//                              -o rist://@0.0.0.0:<9800..9899>?weight=1000
//                              -C <pcr_pid>
//     -> box
//
// The filter -> cutter hop is LOOPBACK UNICAST, not the 238.0.0.0/8 fabric.
// part8_recovery_server binds a multicast port but never joins the group, and
// br-rist is member-less so it comes up NO-CARRIER -- either alone is enough to
// make the cutter read nothing, and both were observed doing exactly that. Part
// 7 needs the fabric because its internal hops are consumed by `tsp -I ip`,
// which rejects a unicast address; Part 8's consumer binds whatever it is
// given, so the reason never applied here. Part 8 has no br-rist dependency.
//
// SEPARATE STORE, and that is the point. Nothing in this file opens, writes or
// locks channels.json, so "no Part 7 record changed" is a property of the code
// rather than a claim about it. The two types do not reference each other at
// all: a service may have both a Part 7 channel and a Part 8 channel, and
// neither knows.
//
// The sender is part8_recovery_server, NOT ristsender: require_selection is on
// by default there and is API-only in librist, the weight-1000 semantics and
// retry path are the deployed ones, and it already builds the PCR -> sequence
// catalogue the anchor query answers from. It runs under the EXISTING
// part8-recovery@ template through the EXISTING part8-unit helper, so there is
// no new unit file and no new sudoers entry; -C reaches it via P8_EXTRA.

require_once dirname(__DIR__) . '/config/config.php';
require_once __DIR__ . '/ts-analysis.php';

if (!defined('P8_CHANNELS_FILE')) define('P8_CHANNELS_FILE', CONFIG_DIR . '/part8-channels.json');
if (!defined('P8_HELPER_BIN'))    define('P8_HELPER_BIN', '/usr/local/sbin/part8-unit');
if (!defined('P8_UNIT_HELPER'))   define('P8_UNIT_HELPER', '/usr/local/sbin/rist-unit');
if (!defined('P8_LIB'))           define('P8_LIB', '/opt/shms/part8-monitor/lib.php');
if (!defined('P8_RUN'))           define('P8_RUN', '/run/part8-recovery');
if (!defined('P8_TSP_BINARY'))    define('P8_TSP_BINARY', '/usr/bin/tsp');
if (!defined('P8_DEFAULT_BUFFER')) define('P8_DEFAULT_BUFFER', 8000);
// VSF TR-06-4 4.3 broadcast PSI. FIXED, not derived from what the analysis
// happened to see: V1 found NIT and TDT/TOT entirely absent on this
// transponder, and a list derived from presence would change the moment a table
// appeared -- on one side only, which is the failure this design exists to
// avoid.
if (!defined('P8_PSI_PIDS'))      define('P8_PSI_PIDS', '0,1,16,17,18,19,20');

class Part8Service
{
    private $file;

    public function __construct()
    {
        $this->file = P8_CHANNELS_FILE;
        if (!file_exists($this->file)) $this->write(['channels' => []]);
    }

    // ---------------------------------------------------------------- store

    private function read()
    {
        $raw  = @file_get_contents($this->file);
        $data = $raw ? json_decode($raw, true) : null;
        if (!is_array($data)) $data = [];
        if (!isset($data['channels']) || !is_array($data['channels'])) $data['channels'] = [];
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

    // Where the boxes are told to connect. Shared with the Part 7 settings so
    // there is one answer to "what address do boxes use", read-only from here.
    private function advertisedIp()
    {
        $raw = @file_get_contents(CONFIG_DIR . '/channels.json');
        $d   = $raw ? json_decode($raw, true) : null;
        $s   = is_array($d) && isset($d['settings']) ? $d['settings'] : [];
        if (!empty($s['recovery_ip'])) return $s['recovery_ip'];
        if (!empty($s['server_ip']))   return $s['server_ip'];
        $out = trim((string)@shell_exec("hostname -I 2>/dev/null | awk '{print \$1}'"));
        return ($out !== '' && filter_var($out, FILTER_VALIDATE_IP)) ? $out : '127.0.0.1';
    }

    // ---------------------------------------------------------------- read

    public function getChannels()
    {
        $out = [];
        foreach ($this->read()['channels'] as $ch) {
            $out[] = $this->decorate($ch);
        }
        return $out;
    }

    public function getChannel($id)
    {
        foreach ($this->read()['channels'] as $ch) {
            if ($ch['id'] === $id) return $this->decorate($ch);
        }
        return null;
    }

    // Everything derived is recomputed on read rather than stored, so a
    // re-analysis cannot leave a stale PID set behind a fresh-looking record.
    private function decorate($ch)
    {
        $ch['status']     = $this->unitState($ch['id']);
        $ch['tsp_status'] = $this->serviceState('ristsender-' . $ch['id'] . '-p8src');
        $ch['rist_url']   = sprintf('rist://%s:%d?buffer=%d', $this->advertisedIp(),
                                    (int)$ch['p8_port'], (int)$ch['buffer']);
        $ch['internal']   = $this->internalAddr($ch) . ':' . (int)$ch['p8_tsp_port'];
        if (!empty($ch['analysis'])) {
            // pcr_pid and pmt_pid are surfaced HERE, resolved for this
            // channel's own service. The GUI must never read
            // analysis.pcr_pid -- that is services[0]'s, i.e. a different
            // service on any shared transponder ingest.
            try {
                $ch['pcr_pid']     = $this->pcrPid($ch);
                $ch['pmt_pid']     = $this->pmtPid($ch);
                $ch['filter_pids'] = $this->filterPids($ch);
            } catch (Exception $e) {
                $ch['filter_error'] = $e->getMessage();
            }
        }
        return $ch;
    }

    // ---------------------------------------------------------------- write

    /**
     * Create. THREE inputs and nothing else: name, input, service id.
     *
     * No marker PID, no uplink URL, no satellite port, no "declare marker in
     * the PMT" -- a Part 8 channel has none of that machinery.
     */
    public function createChannel($input)
    {
        $data = $this->read();

        $name = trim((string)($input['name'] ?? ''));
        if ($name === '') throw new Exception('Channel name is required');
        $id = $this->slug($name);
        foreach ($data['channels'] as $ch) {
            if ($ch['id'] === $id) throw new Exception("A Part 8 channel named '{$name}' already exists");
        }

        $serviceId = $this->validId($input['service_id'] ?? 0);
        foreach ($data['channels'] as $ch) {
            if ((int)$ch['service_id'] === $serviceId) {
                throw new Exception("Service ID {$serviceId} already has a Part 8 channel ('{$ch['name']}')");
            }
        }

        // service_id here is BOTH the analysis selector and the box's lookup
        // key, unlike Part 7 where they are deliberately different. Part 8
        // channels are not remuxed -- the transponder goes out as it came in --
        // so the id in our ingest and the id the box sees are the same number.
        $channel = [
            'id'         => $id,
            'name'       => $name,
            'input_url'  => $this->normaliseUdp($input['input_url'] ?? ''),
            'service_id' => $serviceId,
            'buffer'     => $this->validBuffer($input['buffer'] ?? P8_DEFAULT_BUFFER),
            'created_at' => date('c'),
        ];

        // Ports before anything is written, because the allocator refuses
        // rather than guessing and a refusal must leave no record behind.
        $this->ensurePorts($channel, $data['channels']);

        $data['channels'][] = $channel;
        $this->write($data);

        logMessage('INFO', "part8 channel created: {$id} (service {$serviceId})");
        return $this->decorate($channel);
    }

    public function updateChannel($id, $input)
    {
        $data  = $this->read();
        $found = null;
        foreach ($data['channels'] as &$ch) {
            if ($ch['id'] !== $id) continue;
            if (isset($input['name']) && trim($input['name']) !== '') $ch['name'] = trim($input['name']);
            if (isset($input['input_url']))  $ch['input_url']  = $this->normaliseUdp($input['input_url']);
            if (isset($input['service_id'])) $ch['service_id'] = $this->validId($input['service_id']);
            if (isset($input['buffer']))     $ch['buffer']     = $this->validBuffer($input['buffer']);
            $ch['updated_at'] = date('c');
            $this->ensurePorts($ch, $data['channels']);
            $found = $ch;
        }
        unset($ch);
        if (!$found) throw new Exception("Part 8 channel '{$id}' not found");

        $this->write($data);

        // Only re-generate units for a channel that already has them; creating
        // them as a side effect of an edit would be a surprise.
        if ($this->unitState($id) !== 'unknown' && file_exists('/etc/part8/instances/' . $id . '.env')) {
            $this->writeUnits($found);
            if ($this->unitState($id) === 'active') $this->restart($id);
        }
        return $this->decorate($found);
    }

    public function deleteChannel($id)
    {
        $data   = $this->read();
        $before = count($data['channels']);
        $data['channels'] = array_values(array_filter($data['channels'],
            function ($c) use ($id) { return $c['id'] !== $id; }));
        if (count($data['channels']) === $before) throw new Exception("Part 8 channel '{$id}' not found");

        $this->removeUnits($id);
        $this->write($data);
        logMessage('INFO', "part8 channel deleted: {$id}");
        return true;
    }

    // ---------------------------------------------------------------- analysis

    public function analyseInput($id)
    {
        $ch = $this->getChannel($id);
        if (!$ch) throw new Exception("Part 8 channel '{$id}' not found");

        $parsed = ts_analyse_udp($ch['input_url'], 'p8-' . $id);

        $data = $this->read();
        foreach ($data['channels'] as &$c) {
            if ($c['id'] === $id) $c['analysis'] = $parsed;
        }
        unset($c);
        $this->write($data);

        logMessage('INFO', "part8 analysed {$id}: " . count($parsed['streams']) . ' stream(s)');
        return $parsed;
    }

    /**
     * THIS channel's service in the analysis report, never services[0].
     *
     * The analysis carries a top-level service_id / pmt_pid / pcr_pid, and they
     * describe whichever service TSDuck listed first. On a shared transponder
     * ingest that is a different service from the one this channel selected, so
     * nothing in Part 8 may read them -- it is how the cutter ended up with
     * TLC-GUYANA's PCR PID (0x0021) on an NCN-Guyana channel and never cut.
     *
     * Refuses when the service is not in the report rather than falling back:
     * the fallback IS the bug.
     */
    private function serviceEntry($ch)
    {
        $a = isset($ch['analysis']) && is_array($ch['analysis']) ? $ch['analysis'] : null;
        if (!$a) throw new Exception('No analysis for this channel - Analyse the input first');
        $want = (int)($ch['service_id'] ?? 0);

        foreach (($a['services'] ?? []) as $s) {
            if ((int)($s['id'] ?? 0) === $want) return $s;
        }

        // An older analysis, from before services[] carried pmt_pid/pcr_pid.
        // Only usable when the top-level record demonstrably describes THIS
        // service; otherwise it is exactly the wrong-service data to avoid.
        if ((int)($a['service_id'] ?? 0) === $want && $want !== 0) {
            return ['id'      => $want,
                    'name'    => (string)($a['name'] ?? ''),
                    'pmt_pid' => (int)($a['pmt_pid'] ?? 0),
                    'pcr_pid' => (int)($a['pcr_pid'] ?? 0)];
        }

        $seen = array_map(function ($s) { return (int)($s['id'] ?? 0); }, $a['services'] ?? []);
        throw new Exception(
            "Service {$want} is not in the analysis of this ingest (it reports "
          . (count($seen) ? implode(', ', $seen) : 'none')
          . '). Re-analyse, or correct the Service ID.');
    }

    /**
     * The PID set the headend filters for this channel, derived from analysis.
     *
     * This service's PMT + PCR + its elementary streams + the fixed 4.3 PSI
     * set. Refuses rather than guessing: a silently short list looks like a
     * working channel and drops audio, and a silently WRONG one looks like a
     * working channel and repairs into the wrong bytes.
     */
    public function filterPids($ch)
    {
        $a    = $ch['analysis'];
        $svc  = (int)($ch['service_id'] ?? 0);
        $me   = $this->serviceEntry($ch);
        $pids = array_map('intval', explode(',', P8_PSI_PIDS));

        // From THIS service's PMT declaration, not the report's first service.
        foreach (['pmt_pid', 'pcr_pid'] as $k) {
            if (!empty($me[$k])) $pids[] = (int)$me[$k];
        }

        // An analysis that saw more than one service is an MPTS report, and
        // then "every non-PSI PID" is the whole transponder, not this service.
        // In that case the per-PID service association is required, not
        // optional.
        $multi   = isset($a['services']) && is_array($a['services']) && count($a['services']) > 1;
        $matched = 0;
        foreach (($a['streams'] ?? []) as $s) {
            $owners = isset($s['services']) && is_array($s['services']) ? $s['services'] : null;
            if ($owners === null) {
                if ($multi) continue;              // cannot attribute it - skip
                $pids[] = (int)$s['pid'];          // single-service report
                $matched++;
            } elseif (in_array($svc, array_map('intval', $owners), true)) {
                $pids[] = (int)$s['pid'];
                $matched++;
            }
        }

        if ($multi && $matched === 0) {
            throw new Exception(
                'The analysis reports ' . count($a['services']) . ' services but '
              . 'carries no per-PID service association, so the elementary '
              . 'streams for service ' . $svc . ' cannot be identified. Analyse '
              . 'a single-service input, or upgrade TSDuck so the report names '
              . 'the service each PID belongs to.');
        }
        if (!$matched) throw new Exception('The analysis lists no elementary streams for this service');

        $pids = array_values(array_unique(array_filter($pids,
            function ($p) { return $p >= 0 && $p < 0x1FFF; })));   // never the null PID
        sort($pids);
        return $pids;
    }

    /**
     * The PCR PID the cutter is given, strictly as declared in THIS service's
     * PMT. Never scanned for, never taken from the report's first service.
     */
    public function pcrPid($ch)
    {
        $me = $this->serviceEntry($ch);
        $p  = (int)($me['pcr_pid'] ?? 0);
        if ($p <= 0 || $p >= 0x1FFF) {
            throw new Exception(sprintf(
                'The analysis reports no PCR PID for service %d%s. Without it '
              . 'the cutter has nothing to cut on.',
                (int)($ch['service_id'] ?? 0),
                !empty($me['name']) ? " ({$me['name']})" : ''));
        }
        return $p;
    }

    public function pmtPid($ch)
    {
        return (int)($this->serviceEntry($ch)['pmt_pid'] ?? 0);
    }

    // ---------------------------------------------------------------- ports

    /**
     * Both ports from the Part 8 allocator in part8-monitor/lib.php: it checks
     * four independent sources (part8 instances, the two SHMS channel configs,
     * running Part 7 units and /proc/net) and refuses rather than guessing.
     *
     * BOTH, not just the listen port. The filter -> cutter hop is loopback
     * unicast now, so p8_tsp_port is a real port on 127.0.0.1 that has to be
     * unique across the whole host -- when it was a per-channel multicast group
     * the port could repeat harmlessly, and it no longer can.
     *
     * Allocated together in one pass: p8_alloc_port() reads the stored config,
     * so calling it twice before the first result is written returns the same
     * port both times.
     */
    private function ensurePorts(&$ch, $allChannels)
    {
        $needListen = empty($ch['p8_port']);
        $needTsp    = empty($ch['p8_tsp_port']);
        if (!$needListen && !$needTsp) return;

        if (!is_readable(P8_LIB)) throw new Exception('Part 8 port allocator not found at ' . P8_LIB);
        require_once P8_LIB;

        // Ports already held by our other channels, taken from the list in hand
        // rather than from the file. The allocator reads part8-channels.json at
        // a fixed deployed path, so it is blind to anything not yet flushed
        // there -- and with the loopback hop these ports are real bind targets
        // on 127.0.0.1, where a duplicate means the second channel's cutter
        // silently receives the first channel's stream. Cheap to exclude, and
        // it removes the dependency on that path being right.
        $peer = [];
        foreach ($allChannels as $o) {
            if (($o['id'] ?? null) === $ch['id']) continue;
            foreach (['p8_port', 'p8_tsp_port'] as $k) {
                if (!empty($o[$k])) $peer[(int)$o[$k]] = true;
            }
        }

        $need = (int)$needListen + (int)$needTsp;
        list($cand, $why) = p8_alloc_ports($need + count($peer), $ch['id']);
        if ($cand === null) throw new Exception('Part 8 ports: ' . $why);

        $ports = array_values(array_filter($cand, function ($p) use ($peer) {
            return !isset($peer[$p]);
        }));
        if (count($ports) < $need) {
            throw new Exception('Part 8 ports: not enough free ports once the '
                              . 'other Part 8 channels are excluded');
        }

        if ($needListen) $ch['p8_port']     = (int)array_shift($ports);
        if ($needTsp)    $ch['p8_tsp_port'] = (int)array_shift($ports);
        logMessage('INFO', sprintf('part8 ports for %s: listen=%d loopback=%d',
            $ch['id'], (int)$ch['p8_port'], (int)$ch['p8_tsp_port']));
    }

    /**
     * The filter -> cutter hop. LOOPBACK UNICAST, deliberately.
     *
     * It used to be a 238.0.0.x group on br-rist, and that does not work, for
     * two independent reasons found on the live headend:
     *
     *   - part8_recovery_server binds the port but never joins the group; there
     *     is no IP_ADD_MEMBERSHIP anywhere in it. `ip maddr show br-rist` showed
     *     the Part 7 group joined and the Part 8 group absent, and the cutter
     *     read ts=0 for the whole run.
     *   - br-rist is a member-less bridge and comes up NO-CARRIER, so multicast
     *     to it black-holes until something forces a dummy member up.
     *
     * Part 7 needs the multicast fabric because its internal hops are consumed
     * by `tsp -I ip`, which rejects a unicast address. Part 8's consumer is
     * part8_recovery_server, which binds whatever it is given -- so the reason
     * never applied here, and dropping it removes both failures and the br-rist
     * dependency with them.
     */
    private function internalAddr($ch)
    {
        return '127.0.0.1';
    }

    // ---------------------------------------------------------------- units

    /**
     * filter --pid, never zap. zap regenerates the PAT and SDT and resets the
     * continuity counters on the PSI PIDs (V1 measured all three), which the
     * box would then see as permanent discontinuities on tables it did not
     * lose. filter passes the original bytes through untouched.
     */
    public function buildTspCommand($ch)
    {
        $pids = $this->filterPids($ch);
        $src  = preg_replace('#^udp://@?#', '', $ch['input_url']);

        // -O ip to a UNICAST loopback address. No --ttl: it is meaningless on
        // loopback, and carrying it would imply a fabric that is no longer
        // involved.
        return sprintf('%s -r -I ip %s -P filter --pid %s -O ip %s:%d',
            P8_TSP_BINARY, $src, implode(' --pid ', $pids),
            $this->internalAddr($ch), (int)$ch['p8_tsp_port']);
    }

    // The sender's env file. part8-unit reads this on stdin and installs it as
    // /etc/part8/instances/<id>.env; the existing template expands it.
    public function buildEnvFile($ch)
    {
        return "P8_INPUT=udp://@" . $this->internalAddr($ch) . ':' . (int)$ch['p8_tsp_port'] . "\n"
             . "P8_LISTEN=rist://@0.0.0.0:" . (int)$ch['p8_port']
                 . '?weight=1000&buffer=' . (int)$ch['buffer'] . "\n"
             . "P8_BUFFER_MS=" . (int)$ch['buffer'] . "\n"
             . "P8_RCVBUF=33554432\n"
             . "P8_SOCK_GROUP=www-data\n"
             // -C is the whole Part 8 framing switch. Delete it from this line
             // and the sender is the stock fixed-7 packetiser again.
             . "P8_EXTRA=-C " . $this->pcrPid($ch) . "\n";
    }

    public function writeUnits($ch)
    {
        // Build both first: filterPids() and pcrPid() can refuse, and refusing
        // before anything is installed leaves no trace.
        $tspCmd = $this->buildTspCommand($ch);
        $env    = $this->buildEnvFile($ch);

        list($ok, $out) = $this->helper('create', $ch['id'], $env);
        if (!$ok) throw new Exception("part8-unit create failed: {$out}");
        // The helper reports whether anything outside Part 8 moved. Recorded
        // rather than dropped: this is the isolation evidence.
        logMessage('INFO', "part8 create {$ch['id']}: " . str_replace("\n", ' | ', trim($out)));

        $unit = 'part8-recovery@' . $ch['id'] . '.service';
        $tsp  = 'ristsender-' . $ch['id'] . '-p8src';
        // The tsp stage has to wear a rist* name because rist-unit accepts no
        // other kind. It is Part 8 regardless, and both isolation checks
        // classify -p8src as such so it does not read as a Part 7 unit moving.
        // No rist-mcast-bridge dependency: the hop to the cutter is loopback
        // unicast, so Part 8 needs nothing from the 238.0.0.0/8 fabric. The
        // INGEST is still multicast, but that group is joined by tsp itself and
        // has real members already.
        $tspUnit = "[Unit]\n"
            . "Description=Part 8 service filter - {$ch['name']}\n"
            . "After={$unit}\n"
            . "BindsTo={$unit}\n"
            . "PartOf={$unit}\n\n"
            . "[Service]\n"
            . "Type=simple\n"
            . "ExecStart=" . $tspCmd . "\n"
            . "Restart=always\n"
            . "RestartSec=3\n"
            . "StandardOutput=journal\n"
            . "StandardError=journal\n\n"
            . "[Install]\n"
            . "WantedBy={$unit}\n";

        $tmp = '/tmp/' . $tsp . '.' . getmypid();
        file_put_contents($tmp, $tspUnit);
        exec('sudo ' . P8_UNIT_HELPER . ' install ' . escapeshellarg($tsp) . ' '
             . escapeshellarg($tmp) . ' 2>&1', $o, $rc);
        @unlink($tmp);
        if ($rc !== 0) throw new Exception("Cannot install unit {$tsp}: " . implode(' ', $o));

        $this->systemctl('daemon-reload');
        $this->systemctl('enable', $tsp);      // starts and stops with the sender
        return true;
    }

    public function removeUnits($id)
    {
        $tsp = 'ristsender-' . $id . '-p8src';
        $this->systemctl('stop', $tsp);
        $this->systemctl('disable', $tsp);
        exec('sudo ' . P8_UNIT_HELPER . ' remove ' . escapeshellarg($tsp) . ' 2>&1');
        // remove stops, disables and deletes the env file; it does not delete
        // the shared template, which other instances still use.
        $this->helper('remove', $id);
        $this->systemctl('daemon-reload');
        return true;
    }

    // ---------------------------------------------------------------- control

    public function start($id)
    {
        $ch = $this->getChannel($id);
        if (!$ch) throw new Exception("Part 8 channel '{$id}' not found");
        // Generate on start rather than on create: the units cannot be built
        // before the analysis exists, and this is the first moment they must.
        $this->writeUnits($ch);
        return $this->helper('start', $id)[0];
    }

    public function stop($id)    { return $this->helper('stop',    $id)[0]; }
    public function restart($id) { return $this->helper('restart', $id)[0]; }

    private function helper($action, $id, $stdin = null)
    {
        $cmd = 'sudo ' . P8_HELPER_BIN . ' ' . escapeshellarg($action) . ' ' . escapeshellarg($id);
        if ($stdin !== null) {
            $tmp = tempnam(sys_get_temp_dir(), 'p8env');
            file_put_contents($tmp, $stdin);
            $cmd .= ' < ' . escapeshellarg($tmp);
        }
        exec($cmd . ' 2>&1', $out, $rc);
        if (isset($tmp)) @unlink($tmp);
        return [$rc === 0, implode("\n", $out)];
    }

    private function unitState($id)
    {
        list($ok, $out) = $this->helper('status', $id);
        $lines = array_values(array_filter(array_map('trim', explode("\n", $out)), 'strlen'));
        return $lines ? $lines[0] : 'unknown';
    }

    private function systemctl($action, $unit = null)
    {
        $cmd = 'sudo ' . SYSTEMCTL_BINARY . ' ' . escapeshellarg($action);
        if ($unit !== null) $cmd .= ' ' . escapeshellarg($unit);
        exec($cmd . ' 2>&1', $out, $rc);
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

    // ---------------------------------------------------------------- anchor

    /**
     * One line to the instance's debug socket. The socket is created by the
     * unit template with -g www-data and mode 0660, so this needs no privilege.
     */
    private function debug($id, $line, $timeout = 2)
    {
        $path = P8_RUN . '/' . $id . '/debug.sock';
        if (!file_exists($path)) {
            return [false, "no debug socket at {$path} - is part8-recovery@{$id} running?"];
        }
        $s = @stream_socket_client('unix://' . $path, $errno, $errstr, $timeout);
        if (!$s) return [false, "cannot connect to {$path}: {$errstr}"];
        stream_set_timeout($s, $timeout);
        fwrite($s, $line . "\n");
        $out = '';
        while (!feof($s)) {
            $chunk = fread($s, 8192);
            if ($chunk === false || $chunk === '') break;
            $out .= $chunk;
            if (strlen($out) > 262144) break;
        }
        fclose($s);
        return [true, $out];
    }

    /**
     * The anchor: which RTP sequence carries a given PCR.
     *
     * Answered from the sender's own catalogue -- the same index the retransmit
     * path serves from -- so it cannot disagree with what a NACK would return.
     * The status word passes through verbatim: a box told OUTSIDE_BUFFER can
     * act on that, where a bare number it cannot use is worse than an error.
     */
    public function anchor($id, $pcr, $durationMs = 0)
    {
        $ch = $this->getChannel($id);
        if (!$ch) throw new Exception("Part 8 channel '{$id}' not found");
        $pcrPid = $this->pcrPid($ch);

        list($ok, $raw) = $this->debug($id, sprintf('resolve %d %s %d',
            $pcrPid, (string)$pcr, (int)$durationMs));
        if (!$ok) return ['status' => 'UNAVAILABLE', 'note' => $raw];

        $out = ['status' => 'UNKNOWN', 'pcr_pid' => $pcrPid, 'raw' => trim($raw)];
        if (preg_match('/^([A-Z_]+)/', ltrim($raw), $m)) $out['status'] = $m[1];
        foreach ([
            'actual_pcr' => '/actual_pcr\s*=\s*(\d+)/',
            'start_ext'  => '/start_ext\s*=\s*(\d+)/',
            'start_wire' => '/start_wire\s*=\s*(\d+)/',
            'end_ext'    => '/end_ext\s*=\s*(\d+)/',
            'end_wire'   => '/end_wire\s*=\s*(\d+)/',
            'payloads'   => '/payloads\s*=\s*(\d+)/',
        ] as $k => $re) {
            if (preg_match($re, $raw, $m)) $out[$k] = (int)$m[1];
        }
        return $out;
    }

    public function bounds($id)
    {
        list($ok, $raw) = $this->debug($id, 'bounds');
        if (!$ok) throw new Exception($raw);
        return ['raw' => trim($raw)];
    }

    public function channelIdForService($serviceId)
    {
        foreach ($this->read()['channels'] as $ch) {
            if ((int)$ch['service_id'] === (int)$serviceId) return $ch['id'];
        }
        return null;
    }

    // ---------------------------------------------------------------- recovery

    /**
     * What the boxes are told. Only channels whose sender is actually running
     * are advertised -- a box should never be pointed at a peer that is not
     * listening.
     *
     * These records carry NO Part 7 keys: no marker_pid, no rist_url. The box
     * accepts an entry with part8_rist_url and no rist_url, which is what makes
     * a Part-8-only service reachable without inventing a Part 7 channel for it.
     */
    public function getRecoveryChannels()
    {
        $ip  = $this->advertisedIp();
        $out = [];
        foreach ($this->read()['channels'] as $ch) {
            if ((int)($ch['service_id'] ?? 0) === 0) continue;
            if ($this->unitState($ch['id']) !== 'active') continue;

            $rec = [
                'service_id'     => (int)$ch['service_id'],
                'ts_id'          => (int)($ch['analysis']['ts_id'] ?? 0),
                'name'           => $ch['name'],
                'part8'          => 1,
                'part8_rist_url' => sprintf('rist://%s:%d?buffer=%d',
                                            $ip, (int)$ch['p8_port'], (int)$ch['buffer']),
                // Tells the box to turn ITS cutter on. It must still take the
                // PID from its own PMT -- see part8_server_pcr_pid below.
                'part8_pcr_cut'  => 1,
            ];
            // DIAGNOSTIC ONLY. This is the PCR PID as it reaches us. The box's
            // is what it sees off the dish. They are the same number on a
            // pass-through transponder, which is the only kind Part 8 serves --
            // but the box must still use its own, because that is the PID
            // present in the bytes it is cutting. It warns if they differ.
            try {
                $rec['part8_server_pcr_pid'] = $this->pcrPid($ch);
                $rec['part8_filter_pids']    = $this->filterPids($ch);
            } catch (Exception $e) {
                // Advertise the channel without the derived detail rather than
                // dropping recovery for it; the box can still connect.
                logMessage('WARNING', "part8 recovery record for {$ch['id']}: " . $e->getMessage());
            }
            $out[] = $rec;
        }
        return $out;
    }

    // ---------------------------------------------------------------- helpers

    private function slug($name)
    {
        $s = strtolower(trim($name));
        $s = preg_replace('/[^a-z0-9]+/', '-', $s);
        $s = trim($s, '-');
        $s = $s !== '' ? $s : 'p8-' . substr(md5($name), 0, 6);
        // part8-unit's own token rule: ^[a-z0-9][a-z0-9-]{0,30}$. Checked here
        // so a name that cannot become a unit is refused at creation rather
        // than at the first start.
        if (!preg_match('/^[a-z0-9][a-z0-9-]{0,30}$/', $s)) {
            throw new Exception("'{$name}' does not reduce to a usable id "
                              . "(need 1-31 chars of a-z, 0-9 and '-')");
        }
        return $s;
    }

    private function normaliseUdp($url)
    {
        $url = trim((string)$url);
        if ($url === '') throw new Exception('The transponder ingest address is required');
        if (strpos($url, 'udp://') !== 0) $url = 'udp://' . $url;
        $body = ltrim(substr($url, 6), '@');
        if (!preg_match('/^[0-9a-zA-Z\.\-]+:\d{1,5}$/', $body)) {
            throw new Exception("Invalid UDP address '{$url}' - expected udp://host:port");
        }
        return 'udp://@' . $body;
    }

    private function validId($v)
    {
        if (is_string($v) && stripos($v, '0x') === 0) $v = hexdec(substr($v, 2));
        $v = (int)$v;
        if ($v < 1 || $v > 65535) throw new Exception('Service ID must be between 1 and 65535');
        return $v;
    }

    private function validBuffer($v)
    {
        $v = (int)$v;
        if ($v < 100 || $v > 60000) throw new Exception('RIST buffer must be between 100 and 60000 ms');
        return $v;
    }
}
