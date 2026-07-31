<?php
// api/channels.php - Channel API
//
//   GET    /api/channels.php                 list channels
//   GET    /api/channels.php?id=<id>         single channel
//   GET    /api/channels.php?settings=1      server settings
//   POST   /api/channels.php                 create   {name,input_url,uplink_url,marker_pid}
//   POST   /api/channels.php?action=start&id=<id>     start | stop | restart
//   PUT    /api/channels.php?id=<id>         update
//   DELETE /api/channels.php?id=<id>         delete

require_once dirname(__DIR__) . '/config/config.php';
require_once dirname(__DIR__) . '/services/channel-service.php';

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') { setCORSHeaders(); exit; }
setCORSHeaders();

$svc    = new ChannelService();
$method = $_SERVER['REQUEST_METHOD'];
$id     = $_GET['id'] ?? null;
$action = $_GET['action'] ?? null;

function body()
{
    $raw = file_get_contents('php://input');
    $data = json_decode($raw, true);
    return is_array($data) ? $data : $_POST;
}

try {
    switch ($method) {

        case 'GET':
            if (isset($_GET['settings'])) {
                successResponse($svc->getSettings());
            } elseif (isset($_GET['stats'])) {
                if (!$id) errorResponse('id is required for stats', 400);
                successResponse($svc->getChannelStats($id));
            } elseif ($id) {
                $ch = $svc->getChannel($id);
                if (!$ch) errorResponse("Channel '{$id}' not found", 404);
                successResponse($ch);
            } else {
                successResponse([
                    'channels' => $svc->getChannels(),
                    'settings' => $svc->getSettings(),
                ]);
            }
            break;

        case 'POST':
            if ($action && $id) {
                switch ($action) {
                    case 'start':   $ok = $svc->startChannel($id);   break;
                    case 'stop':    $ok = $svc->stopChannel($id);    break;
                    case 'restart': $ok = $svc->restartChannel($id); break;
                    default: errorResponse("Unknown action '{$action}'", 400);
                }
                if (!$ok) errorResponse("Failed to {$action} channel '{$id}' - see journalctl", 500);
                successResponse($svc->getChannel($id), ucfirst($action) . ' ok');
            } elseif ($action === 'settings') {
                successResponse($svc->saveSettings(body()), 'Settings saved');
            } else {
                successResponse($svc->createChannel(body()), 'Channel created');
            }
            break;

        case 'PUT':
            if (!$id) errorResponse('id is required', 400);
            successResponse($svc->updateChannel($id, body()), 'Channel updated');
            break;

        case 'DELETE':
            if (!$id) errorResponse('id is required', 400);
            $svc->deleteChannel($id);
            successResponse(null, 'Channel deleted');
            break;

        default:
            errorResponse('Method not allowed', 405);
    }
} catch (Exception $e) {
    logMessage('ERROR', 'Channel API error: ' . $e->getMessage(), [
        'method' => $method, 'id' => $id, 'action' => $action,
    ]);
    errorResponse($e->getMessage(), 400);
}
