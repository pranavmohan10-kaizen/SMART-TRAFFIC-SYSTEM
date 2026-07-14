const express = require('express');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = 3000;

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

let lastState = null;

// GET /api/state - stream the traffic engine state
app.get('/api/state', (req, res) => {
    const statePath = path.join(__dirname, 'traffic_state.json');
    fs.readFile(statePath, 'utf8', (err, data) => {
        if (err) {
            // File might not exist yet if traffic_engine hasn't started
            if (lastState) {
                return res.json(lastState);
            }
            return res.status(202).json({ waiting: true });
        }
        try {
            const parsed = JSON.parse(data);
            lastState = parsed;
            res.json(parsed);
        } catch (parseErr) {
            // JSON parsing error could happen if we read during rename (rare but possible on some FS)
            if (lastState) {
                res.json(lastState);
            } else {
                res.status(202).json({ waiting: true });
            }
        }
    });
});

// POST /api/control - send commands to the sensor simulator
app.post('/api/control', (req, res) => {
    const { command } = req.body;
    if (!command) {
        return res.status(400).json({ error: 'Command is required' });
    }

    const controlPath = path.join(__dirname, 'sensor_control.txt');
    const tempPath = controlPath + '.tmp';

    // Write command atomically
    fs.writeFile(tempPath, command + '\n', (writeErr) => {
        if (writeErr) {
            console.error('Error writing control temp file:', writeErr);
            return res.status(500).json({ error: 'Failed to write control file' });
        }

        fs.rename(tempPath, controlPath, (renameErr) => {
            if (renameErr) {
                console.error('Error renaming control file:', renameErr);
                return res.status(500).json({ error: 'Failed to apply control command' });
            }
            res.json({ success: true });
        });
    });
});

// GET /api/logs - read the audit log
app.get('/api/logs', (req, res) => {
    const logPath = path.join(__dirname, 'traffic_log.txt');
    fs.readFile(logPath, 'utf8', (err, data) => {
        if (err) {
            return res.json({ logs: [] });
        }
        const lines = data.trim().split('\n');
        // Get the last 100 lines
        const lastLines = lines.slice(-100);
        res.json({ logs: lastLines });
    });
});

app.listen(PORT, () => {
    console.log(`Smart Traffic Web Server running on http://localhost:${PORT}`);
});
