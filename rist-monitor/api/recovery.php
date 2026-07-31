<?php
// api/recovery.php - Set-top box facing recovery lookup
//
// Fetched by each STB at boot over a DNS name, e.g.
//     GET http://rist-api.example.com/api/recovery.php
//
// Returns every channel that currently has RIST recovery available - i.e.
// configured with a service_id AND whose sender is running right now. A box
// that finds its service_id here switches to the RIST path on zap; anything
// absent stays on the normal tuner -> demux -> decode path.
//
// Read-only. No side effects. Safe to poll.

require_once dirname(__DIR__) . '/config/config.php';
require_once dirname(__DIR__) . '/services/channel-service.php';

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') { setCORSHeaders(); exit; }

// Boxes only ever read - reject anything else plainly.
if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
    setCORSHeaders();
    errorResponse('Method not allowed', 405);
}

setCORSHeaders();

try {
    $svc      = new ChannelService();
    $channels = $svc->getRecoveryChannels();

    // Optional single lookup: ?service_id=1000
    if (isset($_GET['service_id'])) {
        $want = (int)$_GET['service_id'];
        foreach ($channels as $ch) {
            if ($ch['service_id'] === $want) {
                jsonResponse($ch);
            }
        }
        // Not configured, or its sender is not running. 404 is the signal for
        // the box to stay on the normal decode path for this service.
        errorResponse("No RIST recovery for service_id {$want}", 404);
    }

    jsonResponse([
        'server_time' => date('c'),
        'count'       => count($channels),
        'channels'    => $channels,
    ]);

} catch (Exception $e) {
    logMessage('ERROR', 'Recovery API error: ' . $e->getMessage());
    errorResponse('Internal error', 500);
}
