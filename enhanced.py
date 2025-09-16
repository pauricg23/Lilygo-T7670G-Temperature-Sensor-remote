#!/usr/bin/env python3
"""
Enhanced Temperature Dashboard Runner
Combines all the improvements into a single application
"""

import os
import sys
import subprocess
import time
from pathlib import Path

def check_dependencies():
    """Check if required packages are installed"""
    required_packages = [
        'flask', 'flask_socketio', 'eventlet'
    ]
    
    missing_packages = []
    for package in required_packages:
        try:
            __import__(package)
        except ImportError:
            missing_packages.append(package)
    
    if missing_packages:
        print("Missing required packages:")
        for package in missing_packages:
            print(f"  - {package}")
        print("\nInstalling missing packages...")
        subprocess.check_call([sys.executable, '-m', 'pip', 'install'] + missing_packages)
        print("Dependencies installed successfully!")

def create_config_file():
    """Create a configuration file for the enhanced dashboard"""
    config_content = """
# Enhanced Temperature Dashboard Configuration

# Database settings
DATABASE_PATH = "temperature_data.db"

# Email settings for alerts
EMAIL_CONFIG = {
    'smtp_server': 'smtp.gmail.com',
    'smtp_port': 587,
    'email': 'your-email@gmail.com',
    'password': 'your-app-password'
}

# Alert thresholds
DEFAULT_ALERTS = [
    {'sensor': 't1', 'condition': 'above', 'threshold': 30.0},
    {'sensor': 't2', 'condition': 'above', 'threshold': 30.0},
    {'sensor': 't3', 'condition': 'above', 'threshold': 30.0},
    {'sensor': 't1', 'condition': 'below', 'threshold': 15.0},
    {'sensor': 't2', 'condition': 'below', 'threshold': 15.0},
    {'sensor': 't3', 'condition': 'below', 'threshold': 15.0}
]

# WebSocket settings
WEBSOCKET_ENABLED = True
REAL_TIME_UPDATES = True

# Performance settings
CACHE_TIMEOUT = 60  # seconds
MAX_DATA_POINTS = 1000
AUTO_REFRESH_INTERVAL = 30  # seconds

# Security settings
ENABLE_AUTH = False  # Set to True to enable basic authentication
AUTH_USERNAME = "admin"
AUTH_PASSWORD = "temp2024"
"""
    
    with open('config.py', 'w') as f:
        f.write(config_content)
    
    print("Configuration file created: config.py")
    print("Please edit config.py to customize your settings")

def main():
    """Main function to run the enhanced dashboard"""
    print("🌡️  Enhanced Temperature Dashboard")
    print("=" * 50)
    
    # Check dependencies
    print("Checking dependencies...")
    check_dependencies()
    
    # Create config file if it doesn't exist
    if not os.path.exists('config.py'):
        create_config_file()
    
    # Check if enhanced dashboard exists
    if not os.path.exists('temperature_dashboard_enhanced.py'):
        print("Error: temperature_dashboard_enhanced.py not found!")
        print("Please make sure the enhanced dashboard file is in the current directory.")
        return
    
    print("\nStarting Enhanced Temperature Dashboard...")
    print("Features enabled:")
    print("  ✅ Modern responsive UI with dark/light theme")
    print("  ✅ Interactive charts with zoom and pan")
    print("  ✅ Real-time data updates via WebSocket")
    print("  ✅ Advanced statistics and trend analysis")
    print("  ✅ Alert system with email notifications")
    print("  ✅ Historical data comparison")
    print("  ✅ Data export functionality")
    print("  ✅ Performance optimizations with caching")
    print("  ✅ Mobile-responsive design")
    
    print("\nDashboard will be available at:")
    print("  🌐 http://localhost:5050")
    print("  📱 Mobile-friendly interface")
    print("  🔄 Real-time updates every 30 seconds")
    
    print("\nPress Ctrl+C to stop the server")
    print("=" * 50)
    
    try:
        # Import and run the enhanced dashboard
        from temperature_dashboard_enhanced import app
        app.run(host="0.0.0.0", port=5050, debug=False)
    except KeyboardInterrupt:
        print("\n\nShutting down Enhanced Temperature Dashboard...")
        print("Thank you for using the enhanced dashboard!")
    except Exception as e:
        print(f"\nError starting dashboard: {e}")
        print("Please check your configuration and try again.")

if __name__ == "__main__":
    main()
    