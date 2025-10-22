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
            <div class="status-badge" style="background: rgba(55, 65, 81, 0.8);">
                <span>&#128187;</span>
                <span>CPU: <?= $system_health['cpu_usage'] ?? '0' ?>%</span>
            </div>
            <div class="status-badge" style="background: rgba(55, 65, 81, 0.8);">
                <span>&#128225;</span>
                <span><?= $system_health['running'] ?? 0 ?> Active</span>
            </div>
        </div>
    </div>
</header>