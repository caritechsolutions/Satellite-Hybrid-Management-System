<?php
require_once dirname(__DIR__) . '/config/config.php';

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');

try {
    $data = json_decode(file_get_contents(SATELLITES_FILE), true);
    $satellites = $data['satellites'] ?? [];
    
    echo json_encode([
        'error' => false,
        'data' => $satellites
    ]);
    
} catch (Exception $e) {
    echo json_encode([
        'error' => true,
        'message' => $e->getMessage()
    ]);
}
?>
