# 🎙️ ESP32 Audio Summarizer

A powerful, WiFi-enabled audio recording and AI transcription/summarization system running on an **ESP32-S3** microcontroller with dual-core FreeRTOS processing.

## ✨ Features

### 🔴 Recording
- **Continuous 24/7 recording** - Automatically starts on power-on
- **WiFi-independent** - Records even without WiFi connection
- **30-second chunks** - Auto-processes every 30 seconds or manually stop mid-recording
- **I2S PDM Microphone** - 8kHz sample rate, 16-bit mono audio quality
- **Instant start/stop** - One-click controls from web interface

### 🧠 AI Processing
- **Automatic transcription** - Via OpenAI Whisper API
- **AI summarization** - Via ChatGPT (gpt-4o-mini)
- **Parallel processing** - Core 0 records, Core 1 transcribes/summarizes simultaneously
- **No blocking** - Recording continues while processing happens in background

### 🌐 Web Interface
- **Real-time status display** - Live state indicator (Recording/Processing/Ready)
- **Progress bar** - Visual feedback of recording progress
- **Results display** - Instant display of transcription and summary
- **Dark mode** - Toggle for comfortable viewing
- **Responsive design** - Works on desktop, tablet, and mobile

### 🎨 Status Indicators
- **Green LED** - Idle/Standby
- **Red LED** - Recording in progress
- **Blue LED** - Transcribing/Processing
- **RGB PWM Control** - Smooth brightness transitions

### ⚙️ Configuration
- **WiFi Setup** - Easy WiFi credential configuration mode
- **Timezone Support** - Set your local timezone
- **Time Format Options** - 12-hour or 24-hour format
- **Settings Persistence** - All preferences saved to SPIFFS

## 🛠️ Hardware Requirements

### Microcontroller
- **Seeed XIAO ESP32-S3** (or compatible ESP32)
- 4MB Flash memory
- SPIFFS partition for file storage

### Audio Input
- **PDM Microphone** (e.g., SPM0423HD4H)
- Clock Pin: GPIO 42
- Data Pin: GPIO 41

### Visual Output
- **RGB LED** (addressable or separate PWM pins)
- Red Pin: GPIO 5
- Green Pin: GPIO 4
- Blue Pin: GPIO 3

### Power
- USB-C power supply or battery

## 📋 Prerequisites

### Software
- Arduino IDE (or PlatformIO)
- ESP32 Board Support Package
- Required Libraries:
  - `ESP_I2S` - I2S audio interface
  - `WiFi` - WiFi connectivity
  - `ArduinoJson` - JSON parsing
  - `SPIFFS` - File system
  - `WebServer` - HTTP server
  - `HTTPClient` - HTTPS requests
  - `FreeRTOS` - Real-time kernel

### API Keys
Create a `secrets.h` file with:
```cpp
#ifndef SECRETS_H
#define SECRETS_H

#define SSID_NAME "YOUR_WIFI_SSID"
#define SSID_PASSWORD "YOUR_WIFI_PASSWORD"
#define OPENAI_API_KEY "sk-your-openai-key"

#endif
```

## 🚀 Quick Start

### 1. Hardware Setup
- Connect PDM microphone to CLK (GPIO 42) and DATA (GPIO 41)
- Connect RGB LED to pins 3, 4, 5
- Power the ESP32

### 2. Upload Firmware
```bash
# Using Arduino IDE
# 1. Open summarizer.ino
# 2. Select Board: "XIAO ESP32S3"
# 3. Upload
```

### 3. Access Web Interface
- Find device IP from serial monitor or router
- Navigate to `http://<ESP32_IP>` in browser
- Click **START** to begin recording
- Click **STOP** to stop and process

## 🎯 How It Works

### Recording Flow
```
Power On
  ↓
Connect to WiFi
  ↓
Start Recording (Green LED)
  ↓
Collect 30 seconds of audio
  ↓
Queue for Transcription (Blue LED)
  ↓
Continue Recording (back to green)
```

### Processing Flow
```
Audio Chunk Ready
  ↓
Transcribe with Whisper API (Core 1)
  ↓
Summarize with ChatGPT (Core 1)
  ↓
Store Results
  ↓
Update Web Interface
  ↓
Ready for next chunk
```

## 🔧 Web Interface Endpoints

### Status
- `GET /status` - Returns current state, recording progress, and last transcription/summary

### Recording Control
- `POST /start` - Start recording
- `POST /stop` - Stop recording

### Configuration
- `GET /settings` - Retrieve current settings
- `POST /settings` - Update settings (timezone, time format, etc.)
- `POST /wifi/save` - Save WiFi credentials

## 📊 Serial Monitor Output

Real-time updates show:
```
[STARTUP] Simple mode - no recovery processing
[I2S] Microphone initialized successfully
[RECORDING] Recording started automatically on boot
[TRANSCRIPTION] Your transcribed text appears here
[SUMMARY] Your AI-generated summary appears here
```

## 🔐 Security

- **HTTPS Support** - Secure API connections to OpenAI/ChatGPT
- **WiFi Secure** - WPA2 encryption
- **API Keys** - Stored only in `secrets.h` (not committed to repo)
- **SPIFFS Encrypted** - Optional encryption for stored data

## ⚡ Performance

- **Recording Latency**: Minimal (buffered in real-time)
- **Transcription Speed**: 5-15 seconds per 30-second chunk
- **Summary Generation**: 3-8 seconds
- **Memory Usage**: ~480KB for dual buffers + processing overhead
- **WiFi Overhead**: Non-blocking, doesn't interrupt recording

## 🐛 Troubleshooting

### Recording Not Starting
- Check I2S pins are connected correctly
- Verify PDM microphone is powered
- Check serial monitor for initialization errors

### Transcription Not Working
- Verify WiFi is connected (`/status` shows WiFi: Connected)
- Check OpenAI API key in `secrets.h`
- Ensure sufficient audio was recorded (>1 second)

### Web Interface Not Responding
- Check ESP32 IP address from serial monitor
- Ensure device is on same WiFi network
- Try hard refresh (Ctrl+F5)

### Results Not Displaying
- Verify transcription completed (check serial monitor)
- Try stopping mid-recording and waiting 10 seconds
- Refresh webpage to see latest results


## 🔄 Development

### Building from Source
```bash
git clone https://github.com/yourusername/esp32-audio-summarizer.git
cd esp32-audio-summarizer/summarizer
# Create secrets.h with your API keys
# Upload via Arduino IDE
```

### Contributing
1. Fork the repository
2. Create feature branch (`git checkout -b feature/amazing-feature`)
3. Commit changes (`git commit -m 'Add amazing feature'`)
4. Push to branch (`git push origin feature/amazing-feature`)
5. Open Pull Request

## 📜 License

MIT License - See LICENSE file for details

## 🙏 Acknowledgments

- **Seeed Studio** - XIAO ESP32-S3 board
- **OpenAI** - Whisper API for transcription
- **OpenAI** - ChatGPT API for summarization
- **FreeRTOS** - Real-time kernel for ESP32

## 📞 Support

For issues, questions, or suggestions:
- Open an issue on GitHub
- Check existing issues for solutions
- See troubleshooting section above

## 🚀 Future Enhancements

- [ ] Local Whisper model (edge transcription)
- [ ] Voice commands
- [ ] Multi-language support
- [ ] Cloud storage integration
- [ ] Email notifications
- [ ] Advanced audio filters
- [ ] Real-time transcription display
- [ ] Customizable chunk duration

---

**Made with ❤️ for the maker community**
