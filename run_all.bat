@echo off
title Smart Traffic Management System Launcher
echo ========================================================
echo   🚦 LAUNCHING SMART TRAFFIC MANAGEMENT SYSTEM 🚦
echo ========================================================
echo.

:: Clean old runtime logs and states
echo Cleaning temporary states and logs...
if exist live_sensor_data.txt del live_sensor_data.txt
if exist live_sensor_data.txt.tmp del live_sensor_data.txt.tmp
if exist sensor_control.txt del sensor_control.txt
if exist traffic_state.json del traffic_state.json
if exist traffic_state.json.tmp del traffic_state.json.tmp
echo.

:: Check Node modules
if not exist node_modules (
    echo Installing node dependencies...
    call npm install
    echo.
)

:: Start C Simulator in a new window
echo Starting Sensor Simulator (Terminal 1)...
start "Sensor Simulator" cmd /k ".\sensor_sim.exe"

:: Start C Traffic Engine in a new window
echo Starting Traffic Engine (Terminal 2)...
start "Traffic Engine" cmd /k "chcp 65001 && .\traffic_engine.exe"

:: Launch Web Browser
echo Launching Web Dashboard in browser...
timeout /t 2 /nobreak >nul
start http://localhost:3000

:: Start Web Server
echo Starting Node.js Web Backend (Terminal 3)...
npm start
