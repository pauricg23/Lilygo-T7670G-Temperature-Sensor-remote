#!/usr/bin/env python3
"""
File Organization Script for Arduino Projects
Organizes files from the messy 'pi shit' folder into proper structure
"""

import os
import shutil
from pathlib import Path

# Source directory
SOURCE_DIR = "/Users/pauricgrant/Downloads/pi shit/trainerize_ocr"

# Target directories
BASE_DIR = "/Users/pauricgrant/Documents/Arduino_Projects/T-A7670G"
SCRIPTS_DIR = "/Users/pauricgrant/Documents/Scripts"

# File categorization rules
PRODUCTION_FILES = [
    "t7670g_production_lte.ino",
    "t7670g_working_version.ino"
]

TESTING_FILES = [
    "AT-TEST.ino",
    "t7670g_gsm_test.ino",
    "t7670g_minimal_test.ino",
    "t7670g_ultra_minimal.ino",
    "t7670g_ultra_simple.ino",
    "t7670g_simple.ino",
    "minimal_test.ino",
    "test_after_erase.ino"
]

DEVELOPMENT_FILES = [
    "t7670g_cellular_http.ino",
    "t7670g_gsm_http.ino",
    "t7670g_basic.ino",
    "t7670g_complete.ino",
    "t7670g_with_oled_battery.ino"
]

DIAGNOSTIC_FILES = [
    "simbase_diagnostic.ino",
    "simbase_test.ino",
    "network_search.ino",
    "hardware_test.ino"
]

OLD_VERSIONS = [
    "t7670g_no_battery.ino",
    "arduino_no_oled.cpp"
]

PYTHON_FILES = [
    "modded_cor.py"
]

def organize_files():
    """Organize files into proper directory structure"""
    
    print("🗂️  Starting file organization...")
    print("=" * 50)
    
    # Create directories if they don't exist
    for category in ["Production", "Testing", "Development", "Diagnostic", "Old_Versions"]:
        os.makedirs(f"{BASE_DIR}/{category}", exist_ok=True)
    
    # Organize Arduino files
    files_organized = 0
    
    for filename in os.listdir(SOURCE_DIR):
        if filename.endswith('.ino') or filename.endswith('.cpp'):
            source_path = os.path.join(SOURCE_DIR, filename)
            
            if filename in PRODUCTION_FILES:
                target_dir = f"{BASE_DIR}/Production"
                category = "Production"
            elif filename in TESTING_FILES:
                target_dir = f"{BASE_DIR}/Testing"
                category = "Testing"
            elif filename in DEVELOPMENT_FILES:
                target_dir = f"{BASE_DIR}/Development"
                category = "Development"
            elif filename in DIAGNOSTIC_FILES:
                target_dir = f"{BASE_DIR}/Diagnostic"
                category = "Diagnostic"
            elif filename in OLD_VERSIONS:
                target_dir = f"{BASE_DIR}/Old_Versions"
                category = "Old Versions"
            else:
                target_dir = f"{BASE_DIR}/Old_Versions"
                category = "Old Versions (uncategorized)"
            
            target_path = os.path.join(target_dir, filename)
            
            try:
                shutil.copy2(source_path, target_path)
                print(f"✅ {filename} → {category}")
                files_organized += 1
            except Exception as e:
                print(f"❌ Error copying {filename}: {e}")
    
    # Organize Python files
    for filename in os.listdir(SOURCE_DIR):
        if filename.endswith('.py'):
            source_path = os.path.join(SOURCE_DIR, filename)
            target_path = os.path.join(f"{SCRIPTS_DIR}/Python", filename)
            
            try:
                shutil.copy2(source_path, target_path)
                print(f"✅ {filename} → Python Scripts")
                files_organized += 1
            except Exception as e:
                print(f"❌ Error copying {filename}: {e}")
    
    # Handle subdirectories
    for item in os.listdir(SOURCE_DIR):
        item_path = os.path.join(SOURCE_DIR, item)
        if os.path.isdir(item_path):
            target_dir = f"{BASE_DIR}/Old_Versions/{item}"
            try:
                shutil.copytree(item_path, target_dir, dirs_exist_ok=True)
                print(f"✅ {item}/ → Old Versions")
                files_organized += 1
            except Exception as e:
                print(f"❌ Error copying directory {item}: {e}")
    
    print("=" * 50)
    print(f"🎉 Organization complete! {files_organized} items organized.")
    print(f"\n📁 New structure:")
    print(f"   Arduino Projects: {BASE_DIR}")
    print(f"   Python Scripts: {SCRIPTS_DIR}/Python")
    print(f"\n📋 Categories:")
    print(f"   • Production: Ready-to-use scripts")
    print(f"   • Testing: Test and debug scripts")
    print(f"   • Development: Work-in-progress scripts")
    print(f"   • Diagnostic: Network and hardware tests")
    print(f"   • Old Versions: Legacy and backup files")

def create_readme():
    """Create a README file explaining the organization"""
    readme_content = """# T-A7670G Arduino Projects

## Folder Structure

### Production/
Ready-to-use scripts for deployment
- `t7670g_production_lte.ino` - Main production script with LTE, sensors, and deep sleep

### Testing/
Test and debug scripts
- `AT-TEST.ino` - Basic AT command testing
- `t7670g_gsm_test.ino` - GSM connectivity testing
- Various minimal test scripts

### Development/
Work-in-progress scripts
- `t7670g_cellular_http.ino` - Cellular HTTP development
- `t7670g_complete.ino` - Complete feature set
- `t7670g_with_oled_battery.ino` - OLED and battery features

### Diagnostic/
Network and hardware diagnostic tools
- `simbase_diagnostic.ino` - Simbase APN diagnostics
- `network_search.ino` - Network search and signal testing
- `hardware_test.ino` - Hardware component testing

### Old_Versions/
Legacy and backup files
- Previous versions and experimental code
- Subdirectories with older implementations

## Usage

1. **Production**: Use files in Production/ for deployment
2. **Testing**: Use Testing/ files for debugging and validation
3. **Development**: Use Development/ files for new features
4. **Diagnostic**: Use Diagnostic/ files for troubleshooting

## Notes

- All files are organized by function and readiness
- Production files are tested and ready for deployment
- Keep this structure clean for easy navigation
"""
    
    readme_path = os.path.join(BASE_DIR, "README.md")
    with open(readme_path, 'w') as f:
        f.write(readme_content)
    
    print(f"📝 Created README.md in {BASE_DIR}")

if __name__ == "__main__":
    organize_files()
    create_readme()
