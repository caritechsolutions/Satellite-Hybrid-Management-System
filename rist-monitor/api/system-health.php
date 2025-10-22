<?php
require_once dirname(__DIR__) . '/config/config.php';
require_once dirname(__DIR__) . '/services/rist-service.php';

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET');
header('Access-Control-Allow-Headers: Content-Type');

try {
    $ristService = new RistService();
    $health = $ristService->getSystemHealth();

    echo json_encode([
        'error' => false,
        'data' => $health
    ]);

} catch (Exception $e) {
    http_response_code(500);
    echo json_encode([
        'error' => true,
        'message' => $e->getMessage()
    ]);
}
?>