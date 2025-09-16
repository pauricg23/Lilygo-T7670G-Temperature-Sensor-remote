#!/usr/bin/env python3
"""
Fix timezone in temperature database
Converts all UTC timestamps to Ireland local time (UTC+1)
"""

import sqlite3
import os
from datetime import datetime, timedelta

def fix_timezone_in_db():
    db_path = "/Users/pauricgrant/Documents/Documents - Pauric's Mac mini/python files/temperature_data.db"
    
    if not os.path.exists(db_path):
        print(f"Database not found at: {db_path}")
        return
    
    try:
        # Connect to database
        conn = sqlite3.connect(db_path)
        cursor = conn.cursor()
        
        # Check if table exists and get its structure
        cursor.execute("SELECT name FROM sqlite_master WHERE type='table';")
        tables = cursor.fetchall()
        print(f"Tables in database: {tables}")
        
        # Find the temperature data table
        temp_table = None
        for table in tables:
            if 'temperature' in table[0].lower():
                temp_table = table[0]
                break
        
        if not temp_table:
            print("No temperature table found")
            return
        
        print(f"Using table: {temp_table}")
        
        # Get table schema
        cursor.execute(f"PRAGMA table_info({temp_table})")
        columns = cursor.fetchall()
        print(f"Table columns: {columns}")
        
        # Find timestamp column
        timestamp_col = None
        for col in columns:
            if 'timestamp' in col[1].lower() or 'ts' in col[1].lower():
                timestamp_col = col[1]
                break
        
        if not timestamp_col:
            print("No timestamp column found")
            return
        
        print(f"Using timestamp column: {timestamp_col}")
        
        # Get all records
        cursor.execute(f"SELECT rowid, {timestamp_col} FROM {temp_table}")
        records = cursor.fetchall()
        
        print(f"Found {len(records)} records to update")
        
        updated_count = 0
        for rowid, timestamp in records:
            try:
                # Parse the timestamp (assuming format like "2025-09-04T15:24:00")
                if timestamp and 'T' in timestamp:
                    dt = datetime.fromisoformat(timestamp.replace('Z', ''))
                    
                    # Add 1 hour for Ireland timezone (UTC+1)
                    local_dt = dt + timedelta(hours=1)
                    
                    # Format back to ISO string
                    new_timestamp = local_dt.isoformat()
                    
                    # Update the record
                    cursor.execute(f"UPDATE {temp_table} SET {timestamp_col} = ? WHERE rowid = ?", 
                                 (new_timestamp, rowid))
                    updated_count += 1
                    
            except Exception as e:
                print(f"Error updating record {rowid}: {e}")
                continue
        
        # Commit changes
        conn.commit()
        print(f"Successfully updated {updated_count} records")
        
        # Show a few examples
        cursor.execute(f"SELECT {timestamp_col} FROM {temp_table} ORDER BY rowid DESC LIMIT 5")
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
