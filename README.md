# Lilygo T7670G Temperature Sensor Remote

A robust ESP32-based temperature monitoring system using the Lilygo T7670G development board with LTE connectivity, OLED display, and multiple DS18B20 temperature sensors.

## Features

- **Multi-Sensor Temperature Monitoring**: Supports up to 3 DS18B20 temperature sensors
- **LTE Connectivity**: Uses SIMCom A7670G modem for remote data transmission
- **OLED Display**: 128x64 SSD1306 display for local status and readings
- **Battery Monitoring**: Accurate battery percentage calculation from voltage readings
- **Deep Sleep Power Management**: Optimized for long-term battery operation
- **Probe Mode**: 3-minute initial setup mode for configuration and testing
- **PSM (Power Saving Mode)**: Reduces power consumption while maintaining network registration

## Hardware Requirements

- Lilygo T7670G development board
- SIM card with data plan
- 3x DS18B20 temperature sensors
- 128x64 SSD1306 OLED display
- LiPo battery (3.7V)

## Pin Configuration

```
DS18B20 Sensors: GPIO 4 (OneWire bus)
OLED Display: SDA=21, SCL=22 (I2C)
Modem: Hardware UART (GPIO 1/3)
```

## Key Features

### Battery Management
- Voltage-based percentage calculation (ignores modem's unreliable percentage)
- Charging state detection
- Smooth percentage transitions to prevent display jitter
- Median-of-5 sampling for stable readings

### Network Optimization
- Cat-M only operation for better power efficiency
- PSM (Power Saving Mode) to reduce registration attempts
- Proper APN configuration for "simbase" network
- 10-minute backoff on connection failures

### Display System
- Blue header with battery information
- Yellow sensor area with temperature readings
- 3-minute probe mode for initial setup
- Automatic display timeout

## Installation

1. Clone this repository
2. Install PlatformIO
3. Configure your SIM card APN settings
4. Upload the firmware using PlatformIO

## Usage

### First Boot (Probe Mode)
On first boot, the device enters a 3-minute probe mode where it:
- Displays battery status and voltage
- Shows temperature readings from all sensors
- Establishes LTE connection in the background
- Allows configuration and testing

### Normal Operation
After probe mode:
- Device wakes every hour (configurable)
- Reads temperatures from all sensors
- Transmits data via LTE
- Displays status on OLED
- Enters deep sleep for power conservation

## Configuration

### Sleep Duration
Modify `SLEEP_DURATION_SECONDS` in the code to change wake interval:
```cpp
#define SLEEP_DURATION_SECONDS 3600  // 1 hour
```

### Temperature Resolution
Adjust sensor resolution for accuracy vs. speed:
```cpp
sensors.setResolution(12);  // 9, 10, 11, or 12 bits
```

### Network Settings
Ensure your SIM card APN is set to "simbase" or modify in `setupLTE()`:
```cpp
sendATCommand("AT+CGDCONT=1,\"IP\",\"simbase\"", 2000);
```

## File Structure

```
├── src/
│   └── production.ino                    # Main firmware (PlatformIO target)
├── production_working_2025_01_15.ino    # Latest working version
├── working_2025-09-11.ino               # Previous working version
├── working_backup_20250915_100543.ino   # Backup version
├── platformio.ini                       # PlatformIO configuration
├── include/                             # Header files
├── lib/                                # Library dependencies
└── test/                               # Test files
```

## Version History

- **v2025-01-15**: Latest stable version with network optimization and battery fixes
- **v2025-09-11**: Previous working version (backup)
- **v2025-09-15**: Backup version (backup)

## Contributing

This is a working code backup repository. For modifications:
1. Create a new branch
2. Make your changes
3. Test thoroughly
4. Submit a pull request

## License

This project is for personal use. Please respect hardware and network provider terms of service.

## Support

For issues related to:
- **Hardware**: Check Lilygo documentation
- **Network**: Contact your SIM card provider
- **Code**: Review troubleshooting section above
