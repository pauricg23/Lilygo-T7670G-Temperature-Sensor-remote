#!/usr/bin/env python3
"""
Fix timezone in temperature database
Converts any remaining UTC timestamps to Ireland local time (UTC+1)
"""

import sqlite3
import os
from datetime import datetime, timedelta

def fix_timezone_in_db():
    db_path = "/Users/pauricgrant/Documents/Documents - Pauric's Mac mini/python files/temperature_data.db"
    
    try:
        # Connect to database
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        
        # Get all records with timestamps
        cursor.execute("SELECT id, timestamp FROM temperature_readings ORDER BY id")
        records = cursor.fetchall()
        
        print(f"Found {len(records)} records to check")
        
        updated_count = 0
        for record_id, timestamp in records:
            try:
                # Check if timestamp looks like UTC (has 'T' and no space)
                if timestamp and 'T' in timestamp and ' ' not in timestamp:
                    # Parse the timestamp (format like "2025-09-04T15:24:00")
                    dt = datetime.fromisoformat(timestamp.replace('Z', ''))
                    
                    # Add 1 hour for Ireland timezone (UTC+1)
                    local_dt = dt + timedelta(hours=1)
                    
                    # Format back to datetime string
                    new_timestamp = local_dt.strftime('%Y-%m-%d %H:%M:%S')
                    
                    # Update the record
                    cursor.execute("UPDATE temperature_readings SET timestamp = ? WHERE id = ?", 
                                 (new_timestamp, record_id))
                    updated_count += 1
                    
                    if updated_count <= 5:  # Show first 5 updates
                        print(f"Updated {record_id}: {timestamp} -> {new_timestamp}")
                    
            except Exception as e:
                print(f"Error updating record {record_id}: {e}")
                continue
        
        # Commit changes
        conn.commit()
        print(f"Successfully updated {updated_count} records")
        
        # Show recent examples
        cursor.execute("SELECT timestamp FROM temperature_readings ORDER BY id DESC LIMIT 5")
        examples = cursor.fetchall()
        print("Recent timestamps after update:")
        for example in examples:
            print(f"  {example[0]}")
        
    except Exception as e:
        print(f"Error: {e}")
    finally:
        conn.close()

if __name__ == "__main__":
    fix_timezone_in_db()
