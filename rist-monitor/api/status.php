<?php
require_once dirname(__DIR__) . '/config/config.php';
require_once dirname(__DIR__) . '/services/rist-service.php';

setCORSHeaders();

$ristService = new RistService();
$transport_id = $_GET['transport_id'] ?? null;

if ($transport_id) {
    $status = $ristService->getTransportStatus($transport_id);
    successResponse($status);
} else {
    $transports = $ristService->getTransports();
    $statuses = [];
    
    foreach ($transports as $transport) {
        $statuses[$transport['id']] = $ristService->getTransportStatus($transport['id']);
    }
    
    successResponse($statuses);
}
?>
