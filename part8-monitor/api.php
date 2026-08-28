<?php
/**
 * Part 8 management UI - JSON API.
 *
 * Every mutation goes through part8-unit, and every mutation reports whether
 * anything outside Part 8 changed. That report is surfaced to the caller rather
 * than logged and forgotten: "did this break Part 7" is the question the whole
 * deployment is built around.
 */
/*
 * ALWAYS return JSON. A 500 with an empty body tells an operator nothing and
 * sends them to the PHP-FPM log to find out what happened -- which is exactly
 * what happened on the first real-systemd run, and this will not be the last
 * surprise a production host produces. Three layers, because a PHP fatal does
 * not unwind through a catch:
 *
 *   1. output buffering, so a half-written response can be discarded rather
 *      than concatenated with the error;
 *   2. a shutdown handler, which is the ONLY thing that sees a fatal such as a
 *      by-reference argument error or an exhausted memory limit;
 *   3. try/catch(Throwable) around the dispatch for everything that does unwind.
 */
ob_start();

// Warnings and notices become exceptions so they surface as JSON with a
// location, instead of being swallowed or printed into the middle of a document.
set_error_handler(function ($no, $str, $file, $line) {
    if (!(error_reporting() & $no)) return false;       // respect @ suppression
    throw new ErrorException($str, 0, $no, $file, $line);
});

register_shutdown_function(function () {
    $e = error_get_last();
    if (!$e || !in_array($e['type'], [E_ERROR, E_PARSE, E_CORE_ERROR,
                                      E_COMPILE_ERROR, E_USER_ERROR], true)) {
        return;                                          // normal exit
    }
    while (ob_get_level() > 0) ob_end_clean();           // drop partial output
    if (!headers_sent()) {
        http_response_code(500);
        header('Content-Type: application/json');
    }
    echo json_encode([
        'ok'    => false,
        'fatal' => true,
        'error' => sprintf('PHP fatal: %s', $e['message']),
        'where' => sprintf('%s:%d', $e['file'], $e['line']),
        'hint'  => 'This is a bug in the Part 8 UI, not a configuration problem. '
                 . 'The message and location above are enough to fix it.',
    ]);
});

require __DIR__ . '/lib.php';

function out($a, int $code = 200)
{
    while (ob_get_level() > 0) ob_end_clean();
    http_response_code($code);
    header('Content-Type: application/json');
    header('Cache-Control: no-store');
    echo json_encode($a);
    exit;
}

$action = $_REQUEST['action'] ?? 'list';
$name   = (string)($_REQUEST['name'] ?? '');

try {

if ($name !== '' && !p8_valid_name($name))
    out(['ok' => false, 'error' => "invalid instance name '$name' (need ^[a-z0-9][a-z0-9-]{0,30}$)"], 400);

switch ($action) {

case 'list':
    $list = p8_instances();
    $rows = [];
    foreach ($list as $n => $inst) {
        $st    = p8_state($n);
        $stats = ($st['active'] === 'active') ? p8_stats($n) : null;
        $rows[] = [
            'name'     => $n,
            'config'   => $inst,
            'state'    => $st,
            'stats'    => $stats,
            'reachable'=> $stats !== null,
        ];
    }
    out(['ok' => true, 'instances' => $rows, 'now' => time()]);

case 'suggest_port':
    [$p, $err] = p8_alloc_port();
    out($p ? ['ok' => true, 'port' => $p] : ['ok' => false, 'error' => $err]);

case 'create':
case 'update':
    $isNew = ($action === 'create');
    $list  = p8_instances();
    if ($isNew && isset($list[$name])) out(['ok' => false, 'error' => "instance '$name' already exists"], 409);
    if (!$isNew && !isset($list[$name])) out(['ok' => false, 'error' => "no such instance '$name'"], 404);

    $input  = trim((string)($_REQUEST['input'] ?? 'UNCONFIGURED'));
    $buffer = (int)($_REQUEST['buffer_ms'] ?? P8_DEFAULT_BUFFER_MS);
    $rsel   = !empty($_REQUEST['require_selection']);
    $port   = isset($_REQUEST['listen_port']) && $_REQUEST['listen_port'] !== ''
              ? (int)$_REQUEST['listen_port'] : 0;

    if ($buffer < 250 || $buffer > 60000)
        out(['ok' => false, 'error' => 'buffer_ms must be 250-60000'], 400);

    // A port is allocated ONCE and then pinned, so an instance keeps its port
    // across edits. Only ask for a new one when there is none.
    if ($port === 0) $port = (int)($list[$name]['listen_port'] ?? 0);
    if ($port === 0) {
        [$port, $err] = p8_alloc_port($name);
        if (!$port) out(['ok' => false, 'error' => $err], 409);
    } else {
        $why = p8_port_conflicts($port, $name);
        if ($why !== null) out(['ok' => false, 'error' => "port $port refused: $why"], 409);
    }

    // The input is a UDP listener too; a listen/input clash is easy to miss.
    $ip = p8_port_of_url($input);
    if ($ip !== null && $ip === $port)
        out(['ok' => false, 'error' => "input port $ip is the same as the listen port"], 409);
    if ($ip !== null) {
        $used = p8_ports_in_use($name);
        if (isset($used[$ip]))
            out(['ok' => false, 'error' => "input port $ip refused: {$used[$ip]}"], 409);
    }

    $inst = [
        'input'             => $input === '' ? 'UNCONFIGURED' : $input,
        'listen_port'       => $port,
        'buffer_ms'         => $buffer,
        'rcvbuf'            => (int)($list[$name]['rcvbuf'] ?? P8_DEFAULT_RCVBUF),
        'require_selection' => $rsel,
        'created'           => $list[$name]['created'] ?? time(),
    ];

    // Check the store is writable BEFORE the helper writes anything. Otherwise a
    // read-only store leaves an env file in /etc/part8/instances with no registry
    // entry -- an instance systemd can see and the UI cannot, which is worse than
    // a clean refusal.
    $storeDir = dirname(P8_STORE);
    if (!is_dir($storeDir) || !is_writable($storeDir))
        out(['ok' => false, 'error' => "instance store $storeDir is not writable as "
             . p8_whoami() . '. Fix: chown -R www-data ' . $storeDir], 500);

    $r = p8_helper('create', $name, p8_env_body($inst));
    if ($r['rc'] !== 0) out(['ok' => false, 'error' => 'helper failed: ' . $r['out']], 500);

    $list[$name] = $inst;
    if (!p8_save($list)) {
        // Roll back so systemd and the registry cannot disagree.
        p8_helper('remove', $name);
        out(['ok' => false, 'error' => 'could not write the instance store; '
             . 'the partially created instance was removed'], 500);
    }

    out(['ok' => true, 'instance' => $inst, 'isolation' => $r['out']]);

case 'remove':
    $list = p8_instances();
    if (!isset($list[$name])) out(['ok' => false, 'error' => "no such instance '$name'"], 404);
    $r = p8_helper('remove', $name);
    if ($r['rc'] !== 0) out(['ok' => false, 'error' => 'helper failed: ' . $r['out']], 500);
    unset($list[$name]);
    p8_save($list);
    out(['ok' => true, 'isolation' => $r['out']]);

case 'start': case 'stop': case 'restart': case 'enable': case 'disable':
    $list = p8_instances();
    if (!isset($list[$name])) out(['ok' => false, 'error' => "no such instance '$name'"], 404);
    if ($action === 'start' && ($list[$name]['input'] ?? '') === 'UNCONFIGURED')
        out(['ok' => false, 'error' => 'input address is still UNCONFIGURED'], 400);
    $r = p8_helper($action, $name);
    out(['ok' => $r['rc'] === 0, 'output' => $r['out'], 'isolation' => $r['out']]);

case 'journal':
    out(['ok' => true, 'text' => p8_helper('journal', $name, '', '120')['out']]);

case 'snapshot':
    out(['ok' => true, 'text' => p8_helper('snapshot')['out']]);

case 'diag':
    // What this app can actually see AS THE WEB USER. The first real run failed
    // because the container ran as root with the SHMS config absent, and neither
    // was true on the headend; this makes that difference visible before it
    // becomes a 500.
    out(['ok' => true, 'diag' => p8_diagnostics()]);

default:
    out(['ok' => false, 'error' => "unknown action '$action'"], 400);
}

} catch (Throwable $t) {
    // Anything that unwinds: a TypeError, a bad JSON decode, a helper that is
    // not installed. Reported with location, because "500" on its own has cost
    // us a log-diving round trip once already.
    out([
        'ok'    => false,
        'error' => get_class($t) . ': ' . $t->getMessage(),
        'where' => $t->getFile() . ':' . $t->getLine(),
        'action'=> $action,
    ], 500);
}
