<?php
// components/header.php
$current_user_ip = getCurrentUserIP();
$system_health = (new RistService())->getSystemHealth();
?>

<header class="header">
    <div class="header-content">
        <div class="logo">&#128752; RIST Monitor</div>
        <nav class="header-nav">
            <a href="/" class="nav-link">Dashboard</a>
            <a href="/satellites.php" class="nav-link">Satellites</a>
        </nav>
        <div class="status-indicators">
            <div class="status-badge status-satellite">
                <span>&#9673;</span>
                <span>System Active</span>
            </div>
            <div class="status-badge" style="background: rgba(55, 65, 81, 0.8);" id="cpu-badge">
                <span>&#128187;</span>
                <span id="cpu-display">CPU: <?= $system_health['cpu_usage'] ?? '0' ?>%</span>
            </div>
            <div class="status-badge" style="background: rgba(55, 65, 81, 0.8);" id="memory-badge">
                <span>&#128230;</span>
                <span id="memory-display">RAM: <?= $system_health['memory_usage'] ?? '0' ?>%</span>
            </div>
            <div class="status-badge" style="background: rgba(55, 65, 81, 0.8);">
                <span>&#128225;</span>
                <span id="transport-count"><?= $system_health['running'] ?? 0 ?> Active</span>
            </div>
        </div>
    </div>
</header>

<script>
// Update system health stats in real-time
function updateSystemHealth() {
    fetch('/api/system-health.php')
        .then(response => response.json())
        .then(result => {
            if (!result.error && result.data) {
                const data = result.data;

                // Update CPU
                const cpuDisplay = document.getElementById('cpu-display');
                if (cpuDisplay) {
                    cpuDisplay.textContent = `CPU: ${data.cpu_usage}%`;

                    // Change color based on usage
                    const cpuBadge = document.getElementById('cpu-badge');
                    if (data.cpu_usage > 80) {
                        cpuBadge.style.background = 'rgba(239, 68, 68, 0.8)'; // Red
                    } else if (data.cpu_usage > 60) {
                        cpuBadge.style.background = 'rgba(245, 158, 11, 0.8)'; // Orange
                    } else {
                        cpuBadge.style.background = 'rgba(55, 65, 81, 0.8)'; // Normal
                    }
                }

                // Update Memory
                const memoryDisplay = document.getElementById('memory-display');
                if (memoryDisplay) {
                    memoryDisplay.textContent = `RAM: ${data.memory_usage}%`;

                    // Change color based on usage
                    const memoryBadge = document.getElementById('memory-badge');
                    if (data.memory_usage > 80) {
                        memoryBadge.style.background = 'rgba(239, 68, 68, 0.8)'; // Red
                    } else if (data.memory_usage > 60) {
                        memoryBadge.style.background = 'rgba(245, 158, 11, 0.8)'; // Orange
                    } else {
                        memoryBadge.style.background = 'rgba(55, 65, 81, 0.8)'; // Normal
                    }
                }

                // Update transport count
                const transportCount = document.getElementById('transport-count');
                if (transportCount) {
                    transportCount.textContent = `${data.running} Active`;
                }
            }
        })
        .catch(error => {
            console.debug('System health update failed:', error);
        });
}

// Update every 5 seconds
setInterval(updateSystemHealth, 5000);

// Initial update after 1 second
setTimeout(updateSystemHealth, 1000);
</script>