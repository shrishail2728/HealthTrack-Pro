# HealthTrack Pro

**A real-time patient monitoring system powered by ESP32 and FreeRTOS.**

## Features
- Monitors ECG, heart rate, body temperature, and humidity using sensors: MAX30102, AD8232, DHT22, DS18B20.
- Real-time multitasking using FreeRTOS with task prioritization and dual-core task pinning.
- Data visualization and cloud logging via ThingSpeak.
- Python-based ECG waveform plotting for sensor validation.

## Hardware Used
- ESP32 Dev Board
- MAX30102 (Pulse Oximeter & Heart Rate)
- AD8232 (ECG Sensor)
- DHT22 (Temperature & Humidity)
- DS18B20 (Digital Temperature Sensor)
- Jumper Wires, Breadboard

## Software Stack
- PlatformIO (VS Code)
- C++ with FreeRTOS
- Python (for data validation)
- ThingSpeak (Cloud logging)
- Git & GitHub

## Architecture
- FreeRTOS tasks handle each sensor independently using core pinning and queues.
- Sensor data is collected, processed, and published to the cloud.
- Real-time graphs plotted using Python for debugging ECG signals.

## Setup
1. Clone the repository:
git clone https://github.com/mohin22/HealthTrack-Pro.git cd HealthTrack-Pro

2. Open with VS Code + PlatformIO extension.
3. Connect the ESP32 board.
4. Upload the code via PlatformIO.
5. (Optional) Run Python scripts in `/scripts` to plot ECG.

## Folder Structure
- `/src`: Main firmware code
- `/include`: Header files
- `/scripts`: Python utilities (e.g., plot_ecg.py)
- `/docs`: Additional documentation

## Cloud Integration
- Uses ThingSpeak REST API for posting real-time sensor data.
- Visual dashboards created on ThingSpeak channels.

## License
This project is licensed under the MIT License.
