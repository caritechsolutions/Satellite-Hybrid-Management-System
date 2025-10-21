<?php 
require_once "/var/www/html/rist-monitor/config/config.php";
echo "Config loaded successfully!<br>";
echo "Your IP: " . getCurrentUserIP() . "<br>";
echo "Allowed IPs: " . print_r(ALLOWED_IPS, true) . "<br>";
echo "Is allowed: " . (isAllowedIP(getCurrentUserIP()) ? "YES" : "NO");
?>
