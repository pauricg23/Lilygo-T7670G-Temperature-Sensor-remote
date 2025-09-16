#!/usr/bin/env python3
import sqlite3
import os
from datetime import datetime, timedelta

def check_database():
    # Try to find the database
    db_paths = [
        '/Users/pauricgrant/Documents/Documents - Pauric\'s Mac mini/python files/temperature_data.db',
        'temperature_data.db',
        '/Users/pauricgrant/Documents/logs/temperature_data.db'
    ]
    
    db_path = None
    for path in db_paths:
        if os.path.exists(path):
            db_path = path
            break
    
    if not db_path:
        print("❌ Database not found. Tried paths:")
        for path in db_paths:
            print(f"   - {path}")
        return
    
    print(f"✅ Found database: {db_path}")
    
    try:
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        
        # Check table structure
        cursor.execute("PRAGMA table_info(temperature_readings)")
        columns = cursor.fetchall()
        print(f"\n📊 Table structure:")
        for col in columns:
            print(f"   - {col[1]} ({col[2]})")
        
        # Get total count
        cursor.execute("SELECT COUNT(*) FROM temperature_readings")
        total = cursor.fetchone()[0]
        print(f"\n📈 Total records: {total}")
        
        # Get latest 10 records
        cursor.execute("SELECT * FROM temperature_readings ORDER BY id DESC LIMIT 10")
        records = cursor.fetchall()
        print(f"\n🕐 Latest 10 records:")
        print("ID | T1     | T2     | T3     | Timestamp")
        print("-" * 50)
        for record in records:
            print(f"{record[0]:2} | {record[1]:6.2f} | {record[2]:6.2f} | {record[3]:6.2f} | {record[4]}")
        
        # Check for missing values
        cursor.execute("SELECT COUNT(*) FROM temperature_readings WHERE t1 IS NULL OR t2 IS NULL OR t3 IS NULL")
        null_count = cursor.fetchone()[0]
        print(f"\n❌ Records with NULL values: {null_count}")
        
        # Check for constant values (same value for 5+ consecutive readings)
        cursor.execute("""
            SELECT t1, t2, t3, COUNT(*) as count 
            FROM temperature_readings 
            GROUP BY t1, t2, t3 
            HAVING COUNT(*) >= 5
            ORDER BY count DESC
        """)
        constant_values = cursor.fetchall()
        if constant_values:
            print(f"\n🔄 Constant values (5+ consecutive readings):")
            for val in constant_values:
                print(f"   T1={val[0]:.2f}, T2={val[1]:.2f}, T3={val[2]:.2f} (appears {val[3]} times)")
        
        # Check temperature ranges
        cursor.execute("SELECT MIN(t1), MAX(t1), MIN(t2), MAX(t2), MIN(t3), MAX(t3) FROM temperature_readings WHERE t1 IS NOT NULL AND t2 IS NOT NULL AND t3 IS NOT NULL")
        ranges = cursor.fetchone()
        print(f"\n🌡️ Temperature ranges:")
        print(f"   T1: {ranges[0]:.2f}°C to {ranges[1]:.2f}°C (range: {ranges[1]-ranges[0]:.2f}°C)")
        print(f"   T2: {ranges[2]:.2f}°C to {ranges[3]:.2f}°C (range: {ranges[3]-ranges[2]:.2f}°C)")
        print(f"   T3: {ranges[4]:.2f}°C to {ranges[5]:.2f}°C (range: {ranges[5]-ranges[4]:.2f}°C)")
        
        # Check recent data (last 24 hours)
        cursor.execute("""
            SELECT COUNT(*) FROM temperature_readings 
            WHERE timestamp > datetime('now', '-1 day')
        """)
        recent_count = cursor.fetchone()[0]
        print(f"\n⏰ Records in last 24 hours: {recent_count}")
        
        # Check for sensor disconnections (DEVICE_DISCONNECTED_C = -127)
        cursor.execute("SELECT COUNT(*) FROM temperature_readings WHERE t1 = -127 OR t2 = -127 OR t3 = -127")
        disconnected_count = cursor.fetchone()[0]
        print(f"🔌 Records with disconnected sensors: {disconnected_count}")
        
        conn.close()
        
    except Exception as e:
        print(f"❌ Error reading database: {e}")

if __name__ == "__main__":
    check_database()
