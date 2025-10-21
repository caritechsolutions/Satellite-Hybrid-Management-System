<?php
// components/header.php
$current_user_ip = getCurrentUserIP();
$system_health = (new RistService())->getSystemHealth();
?>

<header class="header">
    <div class="header-content">
        <div class="logo">??? RIST Monitor</div>
        <div class="status-indicators">
            <div class="status-badge status-satellite">
                <span>???</span>
                <span>System Active</span>
            </div>
            <div class="status-badge" style="background: rgba(55, 65, 81, 0.8);">
                <span>??</span>
                <span>CPU: <?= $system_health['cpu_usage'] ?? '0' ?>%</span>
            </div>
            <div class="status-badge" style="background: rgba(55, 65, 81, 0.8);">
                <span>??</span>
                <span><?= $system_health['running'] ?? 0 ?> Active</span>
            </div>
        </div>
    </div>
</header>