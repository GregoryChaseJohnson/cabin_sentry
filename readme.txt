ESP32 acts as an OTA client — it pulls firmware from a server (your PC).

You must run a local HTTPS server to serve firmware:

bash
Copy code
python3 pytest_simple_ota.py build 8070 server_certs
The firmware URL is set in menuconfig:

Example Configuration → Firmware Upgrade URL
To trigger OTA manually:


curl http://<ESP32-IP>/trigger_ota
To trigger diagnostics manually:

curl http://<ESP32-IP>/trigger_diag
These HTTP endpoints (/trigger_ota, /trigger_diag) are defined in ota_diag.c and registered by ota_diag_init_no_sntp().

main.c is the active source file and calls the OTA init function.

ESP32 hosts:

OTA + diagnostics server on port 80

Camera stream on port 81

If OTA succeeds, the ESP32 automatically reboots into the new firmware.

The pytest_simple_ota.py script is a ready-to-run HTTPS server that serves your built firmware and waits for requests.

