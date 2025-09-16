#!/usr/bin/env python3
"""
Downloads Folder Organization Script
Organizes files from Downloads into proper categories
"""

import os
import shutil
from pathlib import Path
import re

# Source directory
SOURCE_DIR = "/Users/pauricgrant/Downloads"

# Target directories
BASE_DIR = "/Users/pauricgrant/Documents/Downloads_Organized"

# File categorization rules
CATEGORIES = {
    "Applications": {
        "extensions": [".dmg", ".pkg", ".app", ".exe", ".msi"],
        "keywords": ["installer", "setup", "driver", "cursor", "plex", "qbittorrent"]
    },
    "3D_Printing": {
        "extensions": [".stl", ".3mf", ".gcode", ".obj", ".ply"],
        "keywords": ["3d", "print", "model", "stl", "gcode"]
    },
    "Documents": {
        "extensions": [".pdf", ".doc", ".docx", ".txt", ".rtf", ".odt"],
        "keywords": ["document", "pdf", "contract", "agreement", "instructions"]
    },
    "Archives": {
        "extensions": [".zip", ".rar", ".7z", ".tar", ".gz", ".bz2"],
        "keywords": ["archive", "compressed", "zip", "rar"]
    },
    "Media": {
        "extensions": [".mp4", ".avi", ".mkv", ".mov", ".wmv", ".mp3", ".wav", ".flac", ".jpg", ".jpeg", ".png", ".gif", ".bmp"],
        "keywords": ["video", "audio", "image", "movie", "music", "photo"]
    },
    "Development": {
        "extensions": [".py", ".js", ".html", ".css", ".cpp", ".c", ".h", ".ino", ".json", ".xml", ".yaml", ".yml"],
        "keywords": ["script", "code", "programming", "development", "python", "javascript"]
    },
    "Hardware_Drivers": {
        "extensions": [".dmg", ".pkg"],
        "keywords": ["driver", "ch34", "usb", "serial", "hardware"]
    },
    "Data_Files": {
        "extensions": [".csv", ".xlsx", ".xls", ".json", ".xml", ".sql", ".db"],
        "keywords": ["data", "spreadsheet", "database", "csv", "workout", "muscle"]
    },
    "Projects": {
        "keywords": ["lilygo", "modem", "series", "trainerize", "workout", "pi shit"]
    }
}

def create_directories():
    """Create all necessary directories"""
    for category in CATEGORIES.keys():
        os.makedirs(f"{BASE_DIR}/{category}", exist_ok=True)
    
    # Create subdirectories for specific projects
    os.makedirs(f"{BASE_DIR}/Projects/LilyGo_Modem", exist_ok=True)
    os.makedirs(f"{BASE_DIR}/Projects/Trainerize", exist_ok=True)
    os.makedirs(f"{BASE_DIR}/Projects/Arduino", exist_ok=True)

def categorize_file(filename, filepath):
    """Determine which category a file belongs to"""
    filename_lower = filename.lower()
    extension = os.path.splitext(filename)[1].lower()
    
    # Check for specific projects first
    if "lilygo" in filename_lower or "modem" in filename_lower:
        return "Projects/LilyGo_Modem"
    elif "trainerize" in filename_lower or "workout" in filename_lower or "muscle" in filename_lower:
        return "Projects/Trainerize"
    elif "pi shit" in filename_lower or "arduino" in filename_lower:
        return "Projects/Arduino"
    
    # Check categories
    for category, rules in CATEGORIES.items():
        if category == "Projects":  # Skip projects category for general files
            continue
            
        # Check extensions
        if "extensions" in rules and extension in rules["extensions"]:
            return category
            
        # Check keywords
        if "keywords" in rules:
            for keyword in rules["keywords"]:
                if keyword in filename_lower:
                    return category
    
    # Default category
    return "Miscellaneous"

def organize_files():
    """Organize files from Downloads folder"""
    
    print("🗂️  Starting Downloads organization...")
    print("=" * 60)
    
    create_directories()
    
    files_organized = 0
    directories_organized = 0
    
    for item in os.listdir(SOURCE_DIR):
        item_path = os.path.join(SOURCE_DIR, item)
        
        # Skip hidden files and system files
        if item.startswith('.') or item == "pi shit":
            continue
            
        try:
            if os.path.isfile(item_path):
                category = categorize_file(item, item_path)
                target_dir = f"{BASE_DIR}/{category}"
                target_path = os.path.join(target_dir, item)
                
                # Handle duplicate filenames
                counter = 1
                original_target = target_path
                while os.path.exists(target_path):
                    name, ext = os.path.splitext(original_target)
                    target_path = f"{name}_{counter}{ext}"
                    counter += 1
                
                shutil.move(item_path, target_path)
                print(f"✅ {item} → {category}")
                files_organized += 1
                
            elif os.path.isdir(item_path):
                category = categorize_file(item, item_path)
                target_dir = f"{BASE_DIR}/{category}"
                target_path = os.path.join(target_dir, item)
                
                # Handle duplicate directory names
                counter = 1
                original_target = target_path
                while os.path.exists(target_path):
                    target_path = f"{original_target}_{counter}"
                    counter += 1
                
                shutil.move(item_path, target_path)
                print(f"✅ {item}/ → {category}")
                directories_organized += 1
                
        except Exception as e:
            print(f"❌ Error moving {item}: {e}")
    
    print("=" * 60)
    print(f"🎉 Organization complete!")
    print(f"   📄 Files organized: {files_organized}")
    print(f"   📁 Directories organized: {directories_organized}")
    print(f"   📍 New location: {BASE_DIR}")

def create_readme():
    """Create a README file explaining the organization"""
    readme_content = """# Downloads Organization

## Folder Structure

### Applications/
Installed applications and installers
- DMG files, PKG installers
- Application bundles

### 3D_Printing/
3D printing related files
- STL, 3MF model files
- GCODE print files
- 3D design files

### Documents/
Document files
- PDFs, Word documents
- Text files, contracts
- Instruction manuals

### Archives/
Compressed files
- ZIP, RAR archives
- Compressed folders

### Media/
Media files
- Videos (MP4, AVI, MKV)
- Audio files (MP3, WAV)
- Images (JPG, PNG, GIF)

### Development/
Code and development files
- Python scripts
- Arduino sketches
- Web development files
- Configuration files

### Hardware_Drivers/
Hardware drivers and utilities
- USB drivers (CH34x)
- Serial communication drivers
- Hardware utilities

### Data_Files/
Data and spreadsheet files
- CSV files
- Excel spreadsheets
- Database files
- Workout data

### Projects/
Project-specific folders
- LilyGo_Modem/ - LilyGo modem documentation and examples
- Trainerize/ - Trainerize workout related files
- Arduino/ - Arduino project files

### Miscellaneous/
Files that don't fit other categories

## Usage

- **Applications**: Install and then delete installers
- **3D_Printing**: Keep models and GCODE files organized
- **Documents**: Important documents for reference
- **Development**: Active code projects
- **Projects**: Complete project folders with documentation

## Maintenance

- Regularly clean out old installers
- Archive completed projects
- Keep only active development files
- Delete temporary and duplicate files
"""
    
    readme_path = os.path.join(BASE_DIR, "README.md")
    with open(readme_path, 'w') as f:
        f.write(readme_content)
    
    print(f"📝 Created README.md in {BASE_DIR}")

if __name__ == "__main__":
    organize_files()
    create_readme()
