<?php
/**
 * Part 8 management UI - JSON API.
 *
 * Every mutation goes through part8-unit, and every mutation reports whether
 * anything outside Part 8 changed. That report is surfaced to the caller rather
 * than logged and forgotten: "did this break Part 7" is the question the whole
 * deployment is built around.
 */
require __DIR__ . '/lib.php';

header('Content-Type: application/json');
header('Cache-Control: no-store');

function out($a, int $code = 200) { http_response_code($code); echo json_encode($a); exit; }

$action = $_REQUEST['action'] ?? 'list';
$name   = (string)($_REQUEST['name'] ?? '');

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

    $r = p8_helper('create', $name, p8_env_body($inst));
    if ($r['rc'] !== 0) out(['ok' => false, 'error' => 'helper failed: ' . $r['out']], 500);

    $list[$name] = $inst;
    if (!p8_save($list)) out(['ok' => false, 'error' => 'could not write the instance store'], 500);

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

default:
    out(['ok' => false, 'error' => "unknown action '$action'"], 400);
}
