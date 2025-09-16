from flask import Flask, render_template_string, jsonify, request, send_from_directory
from datetime import datetime, timedelta
import json
import os
import sqlite3
from collections import defaultdict
import statistics
import threading
import time
from functools import wraps
import hashlib
import secrets

# ============ Configuration ============
LOG_DIR = "logs"
DB_PATH = "temperature_data.db"
SECRET_KEY = secrets.token_hex(16)

# ============ Database Setup ============
def init_database():
    """Initialize SQLite database for better performance"""
    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()
    
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS temperature_readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            t1 REAL,
            t2 REAL,
            t3 REAL,
            sensor_status TEXT DEFAULT 'active'
        )
    ''')
    
    cursor.execute('''
        CREATE INDEX IF NOT EXISTS idx_timestamp ON temperature_readings(timestamp)
    ''')
    
    conn.commit()
    conn.close()

# ============ Authentication ============
def require_auth(f):
    @wraps(f)
    def decorated_function(*args, **kwargs):
        auth = request.authorization
        if not auth or not check_auth(auth.username, auth.password):
            return jsonify({'error': 'Authentication required'}), 401
        return f(*args, **kwargs)
    return decorated_function

def check_auth(username, password):
    # Simple authentication - in production, use proper user management
    return username == 'admin' and password == 'temp2024'

# ============ Data Management ============
class TemperatureDataManager:
    def __init__(self):
        self.cache = {}
        self.cache_timeout = 60  # seconds
        init_database()
    
    def add_reading(self, t1, t2, t3, timestamp=None):
        """Add new temperature reading to database"""
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        
        cursor.execute('''
            INSERT INTO temperature_readings (timestamp, t1, t2, t3)
            VALUES (?, ?, ?, ?)
        ''', (self._format_timestamp(timestamp), t1, t2, t3))
        
        conn.commit()
        conn.close()
        

    def _format_timestamp(self, timestamp):
        """Convert ISO timestamp to database format"""
        if not timestamp or timestamp == "null":
            print("Using server timestamp (ESP32 sent null)"); return datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        try:
            # Parse ISO format (2025-09-11T17:25:05) and convert to database format
            dt = datetime.fromisoformat(timestamp.replace('T', ' '))
            return dt.strftime("%Y-%m-%d %H:%M:%S")
        except:
            print("Using server timestamp (ESP32 sent null)"); return datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        # Clear cache
        self.cache.clear()
    
    def get_recent_readings(self, hours=24, limit=1000):
        """Get recent temperature readings with caching"""
        cache_key = f"recent_{hours}_{limit}"
        now = time.time()
        
        if cache_key in self.cache:
            data, timestamp = self.cache[cache_key]
            if now - timestamp < self.cache_timeout:
                return data
        
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        
        cutoff_time = datetime.now() - timedelta(hours=hours)
        cursor.execute('''
            SELECT timestamp, t1, t2, t3
            FROM temperature_readings
            WHERE timestamp >= ?
            ORDER BY timestamp DESC
            LIMIT ?
        ''', (cutoff_time, limit))
        
        data = []
        for row in cursor.fetchall():
            data.append({
                'time': datetime.fromisoformat(row[0]).strftime('%H:%M'),
                'ts': row[0],
                't1': row[1],
                't2': row[2],
                't3': row[3]
            })
        
        conn.close()
        
        # Cache the result
        self.cache[cache_key] = (data, now)
        return data
    
    def get_statistics(self, hours=24):
        """Get temperature statistics for the specified period"""
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        
        cutoff_time = datetime.now() - timedelta(hours=hours)
        cursor.execute('''
            SELECT t1, t2, t3, timestamp
            FROM temperature_readings
            WHERE timestamp >= ?
        ''', (cutoff_time,))
        
        data = cursor.fetchall()
        conn.close()
        
        if not data:
            return {'t1': None, 't2': None, 't3': None}
        
        stats = {}
        for sensor in ['t1', 't2', 't3']:
            values = [row[0] if sensor == 't1' else row[1] if sensor == 't2' else row[2] 
                     for row in data if row[0] is not None or row[1] is not None or row[2] is not None]
            
            # Filter out None values
            values = [v for v in values if v is not None]
            
            if values:
                min_val = min(values)
                max_val = max(values)
                avg_val = statistics.mean(values)
                
                # Find timestamps for min/max
                min_time = next(row[3] for row in data if (row[0] if sensor == 't1' else row[1] if sensor == 't2' else row[2]) == min_val)
                max_time = next(row[3] for row in data if (row[0] if sensor == 't1' else row[1] if sensor == 't2' else row[2]) == max_val)
                
                stats[sensor] = {
                    'min': {'val': min_val, 'time': datetime.fromisoformat(min_time).strftime('%H:%M')},
                    'max': {'val': max_val, 'time': datetime.fromisoformat(max_time).strftime('%H:%M')},
                    'avg': round(avg_val, 2),
                    'current': values[-1] if values else None
                }
            else:
                stats[sensor] = None
        
        return stats

# ============ Flask App ============
app = Flask(__name__)
app.secret_key = SECRET_KEY

data_manager = TemperatureDataManager()

# ============ Routes ============
@app.route("/submit", methods=["POST"])
def submit():
    """Receive temperature data from ESP32"""
    try:
        data = request.get_json(force=True)
        
        # Validate temperature values
        t1 = validate_temp(data.get("t1"))
        t2 = validate_temp(data.get("t2"))
        t3 = validate_temp(data.get("t3"))
        
        if t1 is not None or t2 is not None or t3 is not None:
            data_manager.add_reading(t1, t2, t3, data.get("ts"))
            return jsonify({"status": "ok", "message": "Data recorded successfully"}), 200
        else:
            return jsonify({"status": "error", "message": "No valid temperature data"}), 400
            
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 400

@app.route("/api/data")
def get_data():
    """Get temperature data with optional filtering"""
    hours = request.args.get('hours', 24, type=int)
    limit = request.args.get('limit', 1000, type=int)
    
    data = data_manager.get_recent_readings(hours, limit)
    return jsonify(data)

@app.route("/api/stats")
def get_stats():
    """Get temperature statistics"""
    hours = request.args.get('hours', 24, type=int)
    stats = data_manager.get_statistics(hours)
    return jsonify(stats)

@app.route("/api/health")
def health_check():
    """Health check endpoint"""
    try:
        # Check database connection
        conn = sqlite3.connect(DB_PATH)
        cursor = conn.cursor()
        cursor.execute("SELECT COUNT(*) FROM temperature_readings")
        count = cursor.fetchone()[0]
        conn.close()
        
        return jsonify({
            "status": "healthy",
            "database": "connected",
            "total_readings": count,
            "timestamp": datetime.now().isoformat()
        })
    except Exception as e:
        return jsonify({
            "status": "unhealthy",
            "error": str(e),
            "timestamp": datetime.now().isoformat()
        }), 500

def validate_temp(val):
    """Validate temperature values"""
    try:
        v = float(val)
        if -10 <= v <= 80:
            return v
    except (TypeError, ValueError):
        pass
    return None

# ============ Enhanced HTML Template ============
TEMPLATE = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>🌡️ Advanced Temperature Dashboard</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/date-fns@2.29.3/index.min.js"></script>
    <link href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css" rel="stylesheet">
    <style>
        :root {
            --primary-color: #2563eb;
            --secondary-color: #7c3aed;
            --success-color: #059669;
            --warning-color: #d97706;
            --danger-color: #dc2626;
            --bg-primary: #ffffff;
            --bg-secondary: #f8fafc;
            --bg-tertiary: #e2e8f0;
            --text-primary: #1e293b;
            --text-secondary: #64748b;
            --border-color: #e2e8f0;
            --shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);
            --shadow-lg: 0 10px 15px -3px rgba(0, 0, 0, 0.1);
        }

        [data-theme="dark"] {
            --bg-primary: #0f172a;
            --bg-secondary: #1e293b;
            --bg-tertiary: #334155;
            --text-primary: #f1f5f9;
            --text-secondary: #94a3b8;
            --border-color: #334155;
        }

        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
            background: var(--bg-secondary);
            color: var(--text-primary);
            line-height: 1.6;
            transition: all 0.3s ease;
        }

        .container {
            max-width: 1400px;
            margin: 0 auto;
            padding: 1rem;
        }

        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 2rem;
            padding: 1.5rem;
            background: var(--bg-primary);
            border-radius: 16px;
            box-shadow: var(--shadow);
        }

        .header h1 {
            font-size: 2rem;
            font-weight: 700;
            background: linear-gradient(135deg, var(--primary-color), var(--secondary-color));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
        }

        .controls {
            display: flex;
            gap: 1rem;
            align-items: center;
        }

        .theme-toggle {
            background: var(--bg-tertiary);
            border: none;
            border-radius: 8px;
            padding: 0.5rem;
            cursor: pointer;
            color: var(--text-primary);
            transition: all 0.3s ease;
        }

        .theme-toggle:hover {
            background: var(--border-color);
        }

        .time-selector {
            background: var(--bg-primary);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 0.5rem 1rem;
            color: var(--text-primary);
            cursor: pointer;
        }

        .status-bar {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 1rem;
            margin-bottom: 2rem;
        }

        .status-card {
            background: var(--bg-primary);
            border-radius: 16px;
            padding: 1.5rem;
            box-shadow: var(--shadow);
            border-left: 4px solid var(--primary-color);
            transition: all 0.3s ease;
        }

        .status-card:hover {
            transform: translateY(-2px);
            box-shadow: var(--shadow-lg);
        }

        .status-card h3 {
            font-size: 0.875rem;
            font-weight: 600;
            color: var(--text-secondary);
            margin-bottom: 0.5rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }

        .status-card .value {
            font-size: 2rem;
            font-weight: 700;
            color: var(--text-primary);
            margin-bottom: 0.5rem;
        }

        .status-card .subtitle {
            font-size: 0.875rem;
            color: var(--text-secondary);
        }

        .chart-container {
            background: var(--bg-primary);
            border-radius: 16px;
            padding: 2rem;
            box-shadow: var(--shadow);
            margin-bottom: 2rem;
        }

        .chart-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 1.5rem;
        }

        .chart-title {
            font-size: 1.25rem;
            font-weight: 600;
            color: var(--text-primary);
        }

        .chart-controls {
            display: flex;
            gap: 0.5rem;
        }

        .chart-btn {
            background: var(--bg-tertiary);
            border: none;
            border-radius: 6px;
            padding: 0.5rem;
            cursor: pointer;
            color: var(--text-primary);
            transition: all 0.3s ease;
        }

        .chart-btn:hover, .chart-btn.active {
            background: var(--primary-color);
            color: white;
        }

        .chart-wrapper {
            position: relative;
            height: 400px;
        }

        .stats-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 1.5rem;
            margin-bottom: 2rem;
        }

        .stat-card {
            background: var(--bg-primary);
            border-radius: 16px;
            padding: 1.5rem;
            box-shadow: var(--shadow);
            transition: all 0.3s ease;
        }

        .stat-card:hover {
            transform: translateY(-2px);
            box-shadow: var(--shadow-lg);
        }

        .stat-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 1rem;
        }

        .stat-title {
            font-size: 1.125rem;
            font-weight: 600;
            color: var(--text-primary);
        }

        .stat-icon {
            width: 40px;
            height: 40px;
            border-radius: 10px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 1.25rem;
            color: white;
        }

        .stat-icon.t1 { background: linear-gradient(135deg, #ef4444, #dc2626); }
        .stat-icon.t2 { background: linear-gradient(135deg, #3b82f6, #2563eb); }
        .stat-icon.t3 { background: linear-gradient(135deg, #10b981, #059669); }

        .stat-content {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 1rem;
        }

        .stat-item {
            text-align: center;
        }

        .stat-label {
            font-size: 0.875rem;
            color: var(--text-secondary);
            margin-bottom: 0.25rem;
        }

        .stat-value {
            font-size: 1.5rem;
            font-weight: 700;
            color: var(--text-primary);
        }

        .loading {
            display: flex;
            justify-content: center;
            align-items: center;
            height: 200px;
            color: var(--text-secondary);
        }

        .spinner {
            width: 40px;
            height: 40px;
            border: 4px solid var(--border-color);
            border-top: 4px solid var(--primary-color);
            border-radius: 50%;
            animation: spin 1s linear infinite;
        }

        @keyframes spin {
            0% { transform: rotate(0deg); }
            100% { transform: rotate(360deg); }
        }

        .error {
            background: #fef2f2;
            border: 1px solid #fecaca;
            color: #dc2626;
            padding: 1rem;
            border-radius: 8px;
            margin: 1rem 0;
        }

        .success {
            background: #f0fdf4;
            border: 1px solid #bbf7d0;
            color: #059669;
            padding: 1rem;
            border-radius: 8px;
            margin: 1rem 0;
        }

        @media (max-width: 768px) {
            .container {
                padding: 0.5rem;
            }
            
            .header {
                flex-direction: column;
                gap: 1rem;
                text-align: center;
            }
            
            .controls {
                flex-wrap: wrap;
                justify-content: center;
            }
            
            .status-bar {
                grid-template-columns: 1fr;
            }
            
            .stats-grid {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <!-- Header -->
        <div class="header">
            <h1><i class="fas fa-thermometer-half"></i> Advanced Temperature Dashboard</h1>
            <div class="controls">
                <select class="time-selector" id="timeRange">
                    <option value="1">Last Hour</option>
                    <option value="6">Last 6 Hours</option>
                    <option value="24" selected>Last 24 Hours</option>
                    <option value="168">Last Week</option>
                </select>
                <button class="theme-toggle" id="themeToggle">
                    <i class="fas fa-moon"></i>
                </button>
                <button class="theme-toggle" id="refreshBtn">
                    <i class="fas fa-sync-alt"></i>
                </button>
            </div>
        </div>

        <!-- Status Bar -->
        <div class="status-bar">
            <div class="status-card">
                <h3>System Status</h3>
                <div class="value" id="systemStatus">Online</div>
                <div class="subtitle">Last update: <span id="lastUpdate">--</span></div>
            </div>
            <div class="status-card">
                <h3>Data Points</h3>
                <div class="value" id="dataPoints">--</div>
                <div class="subtitle">Total readings collected</div>
            </div>
            <div class="status-card">
                <h3>Average Temperature</h3>
                <div class="value" id="avgTemp">--</div>
                <div class="subtitle">Across all sensors</div>
            </div>
        </div>

        <!-- Main Chart -->
        <div class="chart-container">
            <div class="chart-header">
                <h2 class="chart-title">Temperature Trends</h2>
                <div class="chart-controls">
                    <button class="chart-btn active" data-chart="line">
                        <i class="fas fa-chart-line"></i>
                    </button>
                    <button class="chart-btn" data-chart="bar">
                        <i class="fas fa-chart-bar"></i>
                    </button>
                    <button class="chart-btn" data-chart="area">
                        <i class="fas fa-chart-area"></i>
                    </button>
                </div>
            </div>
            <div class="chart-wrapper">
                <canvas id="mainChart"></canvas>
            </div>
        </div>

        <!-- Statistics Grid -->
        <div class="stats-grid">
            <div class="stat-card">
                <div class="stat-header">
                    <h3 class="stat-title">Sensor T1</h3>
                    <div class="stat-icon t1">
                        <i class="fas fa-thermometer-half"></i>
                    </div>
                </div>
                <div class="stat-content">
                    <div class="stat-item">
                        <div class="stat-label">Current</div>
                        <div class="stat-value" id="t1Current">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Average</div>
                        <div class="stat-value" id="t1Avg">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Min</div>
                        <div class="stat-value" id="t1Min">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Max</div>
                        <div class="stat-value" id="t1Max">--</div>
                    </div>
                </div>
            </div>

            <div class="stat-card">
                <div class="stat-header">
                    <h3 class="stat-title">Sensor T2</h3>
                    <div class="stat-icon t2">
                        <i class="fas fa-thermometer-half"></i>
                    </div>
                </div>
                <div class="stat-content">
                    <div class="stat-item">
                        <div class="stat-label">Current</div>
                        <div class="stat-value" id="t2Current">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Average</div>
                        <div class="stat-value" id="t2Avg">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Min</div>
                        <div class="stat-value" id="t2Min">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Max</div>
                        <div class="stat-value" id="t2Max">--</div>
                    </div>
                </div>
            </div>

            <div class="stat-card">
                <div class="stat-header">
                    <h3 class="stat-title">Sensor T3</h3>
                    <div class="stat-icon t3">
                        <i class="fas fa-thermometer-half"></i>
                    </div>
                </div>
                <div class="stat-content">
                    <div class="stat-item">
                        <div class="stat-label">Current</div>
                        <div class="stat-value" id="t3Current">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Average</div>
                        <div class="stat-value" id="t3Avg">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Min</div>
                        <div class="stat-value" id="t3Min">--</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Max</div>
                        <div class="stat-value" id="t3Max">--</div>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <script>
        // Global variables
        let mainChart;
        let currentTheme = 'light';
        let currentTimeRange = 24;
        let refreshInterval;

        // Initialize the application
        document.addEventListener('DOMContentLoaded', function() {
            initializeTheme();
            initializeChart();
            initializeEventListeners();
            loadData();
            startAutoRefresh();
        });

        // Theme management
        function initializeTheme() {
            const savedTheme = localStorage.getItem('theme') || 'light';
            setTheme(savedTheme);
        }

        function setTheme(theme) {
            currentTheme = theme;
            document.documentElement.setAttribute('data-theme', theme);
            localStorage.setItem('theme', theme);
            
            const themeIcon = document.getElementById('themeToggle').querySelector('i');
            themeIcon.className = theme === 'dark' ? 'fas fa-sun' : 'fas fa-moon';
        }

        // Chart initialization
        function initializeChart() {
            const ctx = document.getElementById('mainChart').getContext('2d');
            
            mainChart = new Chart(ctx, {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [
                        {
                            label: 'Sensor T1',
                            data: [],
                            borderColor: '#ef4444',
                            backgroundColor: 'rgba(239, 68, 68, 0.1)',
                            borderWidth: 3,
                            pointRadius: 0,
                            pointHoverRadius: 6,
                            tension: 0.4,
                            fill: false
                        },
                        {
                            label: 'Sensor T2',
                            data: [],
                            borderColor: '#3b82f6',
                            backgroundColor: 'rgba(59, 130, 246, 0.1)',
                            borderWidth: 3,
                            pointRadius: 0,
                            pointHoverRadius: 6,
                            tension: 0.4,
                            fill: false
                        },
                        {
                            label: 'Sensor T3',
                            data: [],
                            borderColor: '#10b981',
                            backgroundColor: 'rgba(16, 185, 129, 0.1)',
                            borderWidth: 3,
                            pointRadius: 0,
                            pointHoverRadius: 6,
                            tension: 0.4,
                            fill: false
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    interaction: {
                        mode: 'index',
                        intersect: false
                    },
                    plugins: {
                        legend: {
                            position: 'top',
                            labels: {
                                usePointStyle: true,
                                padding: 20,
                                font: {
                                    size: 14,
                                    weight: '500'
                                }
                            }
                        },
                        tooltip: {
                            backgroundColor: 'rgba(0, 0, 0, 0.8)',
                            titleColor: '#ffffff',
                            bodyColor: '#ffffff',
                            borderColor: '#374151',
                            borderWidth: 1,
                            cornerRadius: 8,
                            displayColors: true,
                            callbacks: {
                                title: function(context) {
                                    return 'Time: ' + context[0].label;
                                },
                                label: function(context) {
                                    return context.dataset.label + ': ' + context.parsed.y.toFixed(2) + '°C';
                                }
                            }
                        }
                    },
                    scales: {
                        x: {
                            grid: {
                                color: 'rgba(0, 0, 0, 0.1)',
                                drawBorder: false
                            },
                            ticks: {
                                color: '#6b7280',
                                font: {
                                    size: 12
                                }
                            }
                        },
                        y: {
                            grid: {
                                color: 'rgba(0, 0, 0, 0.1)',
                                drawBorder: false
                            },
                            ticks: {
                                color: '#6b7280',
                                font: {
                                    size: 12
                                },
                                callback: function(value) {
                                    return value + '°C';
                                }
                            },
                            suggestedMin: 15,
                            suggestedMax: 35
                        }
                    },
                    animation: {
                        duration: 1000,
                        easing: 'easeInOutQuart'
                    }
                }
            });
        }

        // Event listeners
        function initializeEventListeners() {
            // Theme toggle
            document.getElementById('themeToggle').addEventListener('click', function() {
                setTheme(currentTheme === 'light' ? 'dark' : 'light');
            });

            // Time range selector
            document.getElementById('timeRange').addEventListener('change', function() {
                currentTimeRange = parseInt(this.value);
                loadData();
            });

            // Refresh button
            document.getElementById('refreshBtn').addEventListener('click', function() {
                loadData();
            });

            // Chart type buttons
            document.querySelectorAll('.chart-btn').forEach(btn => {
                btn.addEventListener('click', function() {
                    const chartType = this.dataset.chart;
                    changeChartType(chartType);
                    
                    // Update active button
                    document.querySelectorAll('.chart-btn').forEach(b => b.classList.remove('active'));
                    this.classList.add('active');
                });
            });
        }

        // Data loading
        async function loadData() {
            try {
                showLoading();
                
                const [dataResponse, statsResponse] = await Promise.all([
                    fetch(`/api/data?hours=${currentTimeRange}`),
                    fetch(`/api/stats?hours=${currentTimeRange}`)
                ]);

                if (!dataResponse.ok || !statsResponse.ok) {
                    throw new Error('Failed to fetch data');
                }

                const data = await dataResponse.json();
                const stats = await statsResponse.json();

                updateChart(data);
                updateStatistics(stats);
                updateStatusBar(data, stats);
                
                hideLoading();
            } catch (error) {
                console.error('Error loading data:', error);
                showError('Failed to load data. Please try again.');
            }
        }

        // Chart updates
        function updateChart(data) {
            if (!data || data.length === 0) return;

            const labels = data.map(d => d.time).reverse();
            const t1Data = data.map(d => d.t1).reverse();
            const t2Data = data.map(d => d.t2).reverse();
            const t3Data = data.map(d => d.t3).reverse();

            mainChart.data.labels = labels;
            mainChart.data.datasets[0].data = t1Data;
            mainChart.data.datasets[1].data = t2Data;
            mainChart.data.datasets[2].data = t3Data;

            mainChart.update('active');
        }

        function changeChartType(type) {
            const chartTypes = {
                'line': 'line',
                'bar': 'bar',
                'area': 'line'
            };

            mainChart.config.type = chartTypes[type];
            
            if (type === 'area') {
                mainChart.data.datasets.forEach(dataset => {
                    dataset.fill = true;
                });
            } else {
                mainChart.data.datasets.forEach(dataset => {
                    dataset.fill = false;
                });
            }

            mainChart.update();
        }

        // Statistics updates
        function updateStatistics(stats) {
            ['t1', 't2', 't3'].forEach(sensor => {
                const sensorStats = stats[sensor];
                if (sensorStats) {
                    document.getElementById(`${sensor}Current`).textContent = 
                        sensorStats.current ? sensorStats.current.toFixed(1) + '°C' : '--';
                    document.getElementById(`${sensor}Avg`).textContent = 
                        sensorStats.avg ? sensorStats.avg + '°C' : '--';
                    document.getElementById(`${sensor}Min`).textContent = 
                        sensorStats.min ? sensorStats.min.val.toFixed(1) + '°C' : '--';
                    document.getElementById(`${sensor}Max`).textContent = 
                        sensorStats.max ? sensorStats.max.val.toFixed(1) + '°C' : '--';
                } else {
                    document.getElementById(`${sensor}Current`).textContent = '--';
                    document.getElementById(`${sensor}Avg`).textContent = '--';
                    document.getElementById(`${sensor}Min`).textContent = '--';
                    document.getElementById(`${sensor}Max`).textContent = '--';
                }
            });
        }

        // Status bar updates
        function updateStatusBar(data, stats) {
            document.getElementById('dataPoints').textContent = data.length;
            document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
            
            // Calculate average temperature
            const allTemps = [];
            data.forEach(d => {
                if (d.t1 !== null) allTemps.push(d.t1);
                if (d.t2 !== null) allTemps.push(d.t2);
                if (d.t3 !== null) allTemps.push(d.t3);
            });
            
            if (allTemps.length > 0) {
                const avg = allTemps.reduce((a, b) => a + b, 0) / allTemps.length;
                document.getElementById('avgTemp').textContent = avg.toFixed(1) + '°C';
            } else {
                document.getElementById('avgTemp').textContent = '--';
            }
        }

        // UI helpers
        function showLoading() {
            // Could add loading indicators here
        }

        function hideLoading() {
            // Hide loading indicators
        }

        function showError(message) {
            // Could show error messages here
            console.error(message);
        }

        // Auto refresh
        function startAutoRefresh() {
            refreshInterval = setInterval(loadData, 30000); // Refresh every 30 seconds
        }

        function stopAutoRefresh() {
            if (refreshInterval) {
                clearInterval(refreshInterval);
            }
        }

        // Cleanup on page unload
        window.addEventListener('beforeunload', stopAutoRefresh);
    </script>
</body>
</html>
"""

@app.route("/")
def index():
    return render_template_string(TEMPLATE)

if __name__ == "__main__":
    # Migrate existing JSON data to database
    def migrate_json_data():
        if os.path.exists(LOG_DIR):
            for filename in os.listdir(LOG_DIR):
                if filename.endswith('.json'):
                    filepath = os.path.join(LOG_DIR, filename)
                    try:
                        with open(filepath, 'r') as f:
                            data = json.load(f)
                            for entry in data:
                                data_manager.add_reading(
                                    entry.get('t1'),
                                    entry.get('t2'),
                                    entry.get('t3')
                                )
                        print(f"Migrated data from {filename}")
                    except Exception as e:
                        print(f"Error migrating {filename}: {e}")

    # Run migration in background
    migration_thread = threading.Thread(target=migrate_json_data)
    migration_thread.daemon = True
    migration_thread.start()

    app.run(host="0.0.0.0", port=5050, debug=True)
