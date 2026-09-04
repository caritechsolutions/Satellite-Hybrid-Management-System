<?php
// api/part8.php - Part 8 recovery channels.
//
//   GET    ?                       list
//   GET    ?id=<id>                one
//   GET    ?analyse=1&id=<id>      run TSDuck against the ingest
//   GET    ?anchor=1&id=<id>&pcr=N which RTP sequence carries this PCR
//   GET    ?bounds=1&id=<id>       what the catalogue currently holds
//   POST   ?                       create {name, input_url, service_id}
//   POST   ?action=start|stop|restart&id=<id>
//   PUT    ?id=<id>                update
//   DELETE ?id=<id>                delete
//
// Deliberately separate from api/channels.php. That one is Part 7 and knows
// nothing about this; the two stores never meet.

require_once dirname(__DIR__) . '/config/config.php';
require_once dirname(__DIR__) . '/services/part8-service.php';

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') { setCORSHeaders(); exit; }
setCORSHeaders();

$svc    = new Part8Service();
$method = $_SERVER['REQUEST_METHOD'];
$id     = $_GET['id'] ?? null;
$action = $_GET['action'] ?? null;

function p8body()
{
    $raw  = file_get_contents('php://input');
    $data = json_decode($raw, true);
    return is_array($data) ? $data : $_POST;
}

try {
    switch ($method) {

        case 'GET':
            if (isset($_GET['analyse'])) {
                if (!$id) errorResponse('id is required to analyse', 400);
                successResponse($svc->analyseInput($id));
            } elseif (isset($_GET['anchor'])) {
                if (!$id) errorResponse('id is required', 400);
                if (!isset($_GET['pcr'])) errorResponse('pcr is required', 400);
                successResponse($svc->anchor($id, $_GET['pcr'], (int)($_GET['duration_ms'] ?? 0)));
            } elseif (isset($_GET['bounds'])) {
                if (!$id) errorResponse('id is required', 400);
                successResponse($svc->bounds($id));
            } elseif ($id) {
                $ch = $svc->getChannel($id);
                if (!$ch) errorResponse("Part 8 channel '{$id}' not found", 404);
                successResponse($ch);
            } else {
                successResponse(['channels' => $svc->getChannels()]);
            }
            break;

        case 'POST':
            if ($action && $id) {
                switch ($action) {
                    case 'start':   $ok = $svc->start($id);   break;
                    case 'stop':    $ok = $svc->stop($id);    break;
                    case 'restart': $ok = $svc->restart($id); break;
                    default: errorResponse("Unknown action '{$action}'", 400);
                }
                if (!$ok) errorResponse("Failed to {$action} Part 8 channel '{$id}' - see journalctl", 500);
                successResponse($svc->getChannel($id), ucfirst($action) . ' ok');
            } else {
                successResponse($svc->createChannel(p8body()), 'Part 8 channel created');
            }
            break;

        case 'PUT':
            if (!$id) errorResponse('id is required', 400);
            successResponse($svc->updateChannel($id, p8body()), 'Part 8 channel updated');
            break;

        case 'DELETE':
            if (!$id) errorResponse('id is required', 400);
            $svc->deleteChannel($id);
            successResponse(null, 'Part 8 channel deleted');
            break;

        default:
            errorResponse('Method not allowed', 405);
    }
} catch (Exception $e) {
    logMessage('ERROR', 'Part 8 API error: ' . $e->getMessage(), [
        'method' => $method, 'id' => $id, 'action' => $action,
    ]);
    errorResponse($e->getMessage(), 400);
}
