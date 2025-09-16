#!/usr/bin/env python3
import sqlite3
import os
import glob

# Try to find the database file
possible_paths = [
    "/Users/pauricgrant/Documents/Documents - Pauric's Mac mini/python files/temperature_data.db",
    "/Users/pauricgrant/Documents/python files/temperature_data.db",
    "/Users/pauricgrant/Documents/temperature_data.db"
]

# Also search for any .db files in Documents
search_paths = glob.glob("/Users/pauricgrant/Documents/**/*.db", recursive=True)
possible_paths.extend(search_paths)

print("Searching for temperature database...")
for path in possible_paths:
    if os.path.exists(path):
        print(f"Found database: {path}")
        try:
            conn = sqlite3.connect(path)
            cursor = conn.cursor()
            cursor.execute("SELECT name FROM sqlite_master WHERE type='table';")
            tables = cursor.fetchall()
            print(f"  Tables: {tables}")
            
            # Check if it has temperature data
            for table in tables:
                if 'temperature' in table[0].lower():
                    cursor.execute(f"SELECT COUNT(*) FROM {table[0]}")
                    count = cursor.fetchone()[0]
                    print(f"  {table[0]} has {count} records")
                    
                    # Show recent timestamps
                    cursor.execute(f"SELECT * FROM {table[0]} ORDER BY rowid DESC LIMIT 3")
                    recent = cursor.fetchall()
                    print(f"  Recent records: {recent}")
            
            conn.close()
            break
        except Exception as e:
            print(f"  Error accessing {path}: {e}")
    else:
        print(f"Not found: {path}")
