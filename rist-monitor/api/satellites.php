<?php
require_once dirname(__DIR__) . '/config/config.php';

header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

// Handle preflight requests
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(200);
    exit;
}

try {
    // Get request method and path
    $method = $_SERVER['REQUEST_METHOD'];

    // Get satellite ID from query string or path
    $satelliteId = $_GET['id'] ?? '';
    if (empty($satelliteId)) {
        $path = $_SERVER['PATH_INFO'] ?? '';
        $satelliteId = trim($path, '/');
    }

    // Load satellites data
    $satellitesFile = SATELLITES_FILE;
    $data = json_decode(file_get_contents($satellitesFile), true);
    $satellites = $data['satellites'] ?? [];

    switch ($method) {
        case 'GET':
            if ($satelliteId) {
                // Get single satellite
                $satellite = null;
                foreach ($satellites as $sat) {
                    if ($sat['id'] === $satelliteId) {
                        $satellite = $sat;
                        break;
                    }
                }

                if ($satellite) {
                    echo json_encode([
                        'error' => false,
                        'data' => $satellite
                    ]);
                } else {
                    http_response_code(404);
                    echo json_encode([
                        'error' => true,
                        'message' => 'Satellite not found'
                    ]);
                }
            } else {
                // Get all satellites
                echo json_encode([
                    'error' => false,
                    'data' => $satellites
                ]);
            }
            break;

        case 'POST':
            // Create new satellite
            $input = json_decode(file_get_contents('php://input'), true);

            // Generate ID from name
            $id = strtolower(preg_replace('/[^a-zA-Z0-9]/', '_', $input['name']));

            // Check if ID already exists
            foreach ($satellites as $sat) {
                if ($sat['id'] === $id) {
                    http_response_code(400);
                    echo json_encode([
                        'error' => true,
                        'message' => 'Satellite with this name already exists'
                    ]);
                    exit;
                }
            }

            $newSatellite = [
                'id' => $id,
                'name' => $input['name'],
                'position' => $input['position'],
                'frequency' => $input['frequency'],
                'symbol_rate' => $input['symbol_rate'],
                'bitrate' => $input['bitrate'],
                'status' => $input['status']
            ];

            $satellites[] = $newSatellite;

            // Save to file
            $data['satellites'] = $satellites;
            file_put_contents($satellitesFile, json_encode($data, JSON_PRETTY_PRINT));

            echo json_encode([
                'error' => false,
                'data' => $newSatellite,
                'message' => 'Satellite created successfully'
            ]);
            break;

        case 'PUT':
            // Update satellite
            if (!$satelliteId) {
                http_response_code(400);
                echo json_encode([
                    'error' => true,
                    'message' => 'Satellite ID is required'
                ]);
                exit;
            }

            $input = json_decode(file_get_contents('php://input'), true);
            $found = false;

            for ($i = 0; $i < count($satellites); $i++) {
                if ($satellites[$i]['id'] === $satelliteId) {
                    $satellites[$i] = array_merge($satellites[$i], [
                        'name' => $input['name'],
                        'position' => $input['position'],
                        'frequency' => $input['frequency'],
                        'symbol_rate' => $input['symbol_rate'],
                        'bitrate' => $input['bitrate'],
                        'status' => $input['status']
                    ]);
                    $found = true;
                    break;
                }
            }

            if (!$found) {
                http_response_code(404);
                echo json_encode([
                    'error' => true,
                    'message' => 'Satellite not found'
                ]);
                exit;
            }

            // Save to file
            $data['satellites'] = $satellites;
            file_put_contents($satellitesFile, json_encode($data, JSON_PRETTY_PRINT));

            echo json_encode([
                'error' => false,
                'data' => $satellites[$i],
                'message' => 'Satellite updated successfully'
            ]);
            break;

        case 'DELETE':
            // Delete satellite
            if (!$satelliteId) {
                http_response_code(400);
                echo json_encode([
                    'error' => true,
                    'message' => 'Satellite ID is required'
                ]);
                exit;
            }

            $found = false;
            $newSatellites = [];

            foreach ($satellites as $sat) {
                if ($sat['id'] === $satelliteId) {
                    $found = true;
                } else {
                    $newSatellites[] = $sat;
                }
            }

            if (!$found) {
                http_response_code(404);
                echo json_encode([
                    'error' => true,
                    'message' => 'Satellite not found'
                ]);
                exit;
            }

            // Save to file
            $data['satellites'] = $newSatellites;
            file_put_contents($satellitesFile, json_encode($data, JSON_PRETTY_PRINT));

            echo json_encode([
                'error' => false,
                'message' => 'Satellite deleted successfully'
            ]);
            break;

        default:
            http_response_code(405);
            echo json_encode([
                'error' => true,
                'message' => 'Method not allowed'
            ]);
            break;
    }

} catch (Exception $e) {
    http_response_code(500);
    echo json_encode([
        'error' => true,
        'message' => $e->getMessage()
    ]);
}
?>
