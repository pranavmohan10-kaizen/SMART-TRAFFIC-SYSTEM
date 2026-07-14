// Global State Cache
let currentState = null;
let activeOverrideLane = null;
let logPollInterval = null;

// Initialize Web Socket or Polling
document.addEventListener('DOMContentLoaded', () => {
    // Start polling the API for traffic system state
    setInterval(pollSystemState, 500);

    // Initial log load and poll
    refreshLogs();
    logPollInterval = setInterval(refreshLogs, 2000);
});

// Poll /api/state
async function pollSystemState() {
    try {
        const response = await fetch('/api/state');
        if (response.status === 202) {
            // Waiting for C binaries to start
            showWaitingScreen(true);
            return;
        }
        
        if (!response.ok) {
            throw new Error('Network response not ok');
        }

        const state = await response.json();
        
        if (state.waiting) {
            showWaitingScreen(true);
            return;
        }

        showWaitingScreen(false);
        currentState = state;
        updateUI(state);
    } catch (error) {
        console.error('Error polling state:', error);
        // Show waiting screen if backend is unreachable
        showWaitingScreen(true);
    }
}

// Show/Hide waiting splash
function showWaitingScreen(show) {
    const splash = document.getElementById('waiting-screen');
    const app = document.querySelector('.app-container');
    
    if (show) {
        splash.classList.remove('hidden');
        splash.style.display = 'flex';
        app.classList.remove('ready');
    } else {
        splash.classList.add('hidden');
        splash.style.display = 'none';
        app.classList.add('ready');
    }
}

// Update Dashboard UI Elements
function updateUI(state) {
    // 1. Update Header Stats
    const modeEl = document.getElementById('stat-mode');
    modeEl.textContent = state.mode;
    
    // Style Mode text based on active mode
    modeEl.className = 'stat-value';
    if (state.mode === 'EMERGENCY') {
        modeEl.classList.add('text-red');
    } else if (state.mode === 'NIGHT') {
        modeEl.classList.add('text-blue');
    } else {
        modeEl.classList.add('text-green');
    }

    // Active Scenario Strategy
    const strategyName = document.getElementById('active-scenario-name');
    if (state.mode === 'NIGHT') {
        strategyName.textContent = 'QUIET NIGHT MODE';
        strategyName.className = 'text-blue';
    } else if (state.mode === 'EMERGENCY') {
        strategyName.textContent = 'EMERGENCY OVERRIDE';
        strategyName.className = 'text-red';
    } else {
        // If we are in normal mode, the simulator can be random, rush, or manual
        // We'll read the logs or assume scenario based on the active button if manual
        // Actually, we can check if any lane is manual, or we can just display Normal
        strategyName.textContent = 'ADAPTIVE SIGNAL FLOW';
        strategyName.className = 'text-green';
    }

    // Timer and phase
    document.getElementById('stat-phase').textContent = state.phase;
    document.getElementById('stat-timer').textContent = `${state.remaining_time}/${state.total_time}s`;
    
    const progressPercent = state.total_time > 0 
        ? ((state.total_time - state.remaining_time) / state.total_time) * 100 
        : 0;
    document.getElementById('stat-progress').style.width = `${progressPercent}%`;

    document.getElementById('stat-cycle').textContent = `#${state.cycle}`;

    // 2. Update Junction Center
    const activeLaneName = document.getElementById('active-lane-name');
    const junctionCenter = document.querySelector('.junction-center');
    if (state.active_lane >= 0) {
        const activeLaneChar = state.lanes[state.active_lane].name;
        activeLaneName.textContent = activeLaneChar;
        junctionCenter.classList.add('active-glow');
        
        // Dynamic glow color
        if (state.phase === 'YELLOW' || state.phase === 'AMBER') {
            junctionCenter.style.boxShadow = '0 0 35px rgba(245, 166, 35, 0.6)';
            junctionCenter.style.borderColor = 'var(--color-yellow)';
        } else {
            junctionCenter.style.boxShadow = '0 0 35px var(--color-green-glow)';
            junctionCenter.style.borderColor = 'var(--color-green)';
        }
    } else {
        activeLaneName.textContent = '--';
        junctionCenter.classList.remove('active-glow');
        junctionCenter.style.boxShadow = 'none';
        junctionCenter.style.borderColor = 'var(--border-color)';
    }

    // 3. Update Lanes
    state.lanes.forEach(lane => {
        const roadEl = document.querySelector(`.road-${lane.name.toLowerCase()}`);
        if (!roadEl) return;

        // Vehicle Count Badge
        const vehEl = document.getElementById(`veh-${lane.name}`);
        vehEl.textContent = `${lane.vehicles} Cars`;
        vehEl.className = 'badge vehicles-badge';
        
        if (lane.vehicles >= 60) {
            vehEl.classList.add('severe');
        } else if (lane.vehicles >= 16) {
            vehEl.classList.add('medium');
        } else {
            vehEl.classList.add('low');
        }

        // Pedestrian Count Badge
        const pedEl = document.getElementById(`ped-${lane.name}`);
        pedEl.textContent = `${lane.pedestrians} Peds`;

        // Emergency Siren overlay
        const sirenEl = document.getElementById(`siren-${lane.name}`);
        if (lane.emergency) {
            sirenEl.style.display = 'block';
            roadEl.classList.add('emergency-active');
        } else {
            sirenEl.style.display = 'none';
            roadEl.classList.remove('emergency-active');
        }

        // Traffic Light bulbs
        const lightEl = document.getElementById(`light-${lane.name}`);
        const bulbs = lightEl.querySelectorAll('.bulb');
        bulbs.forEach(b => b.classList.remove('active'));

        if (lane.skipped) {
            // In night mode dynamic skip, keep them all off or dim red
            lightEl.querySelector('.bulb.red').classList.add('active'); // show red if skipped
        } else if (lane.signal === 'GREEN') {
            lightEl.querySelector('.bulb.green').classList.add('active');
        } else if (lane.signal === 'YELLOW' || lane.signal === 'AMBER') {
            lightEl.querySelector('.bulb.yellow').classList.add('active');
        } else {
            lightEl.querySelector('.bulb.red').classList.add('active');
        }

        // Crosswalk Walk Sign
        const walkEl = document.getElementById(`walk-${lane.name}`);
        if (lane.walk_active) {
            walkEl.textContent = 'WALK';
            walkEl.classList.add('walk-active');
        } else {
            walkEl.textContent = 'DONT WALK';
            walkEl.classList.remove('walk-active');
        }
    });

    // 4. Update Priority Queue List
    const priorityList = document.getElementById('priority-list');
    priorityList.innerHTML = '';
    
    state.priority_queue.forEach((item, index) => {
        const prioItem = document.createElement('div');
        prioItem.className = 'priority-item';
        
        // Find full density tier for this lane count
        const laneDetails = state.lanes.find(l => l.name === item.name);
        const densityClass = laneDetails ? laneDetails.density.toLowerCase() : 'low';
        
        prioItem.innerHTML = `
            <span class="prio-rank">#${index + 1}</span>
            <span class="prio-name">Lane ${item.name}</span>
            <span class="badge vehicles-badge ${densityClass}">${item.vehicles} Cars (${laneDetails.density})</span>
        `;
        priorityList.appendChild(prioItem);
    });

    // 5. Update Live Decision Feed
    const feedContainer = document.getElementById('feed-messages');
    feedContainer.innerHTML = '';
    
    state.live_feed.forEach(msg => {
        const feedItem = document.createElement('div');
        feedItem.className = `feed-item ${msg.color || 'white'}`;
        feedItem.textContent = msg.text;
        feedContainer.appendChild(feedItem);
    });
    
    // Auto-scroll feed to bottom
    feedContainer.scrollTop = feedContainer.scrollHeight;
}

// API - Set Scenario Strategy
async function setScenario(type) {
    try {
        const response = await fetch('/api/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ command: `SCENARIO ${type}` })
        });
        if (response.ok) {
            // Update active state visual style of buttons
            const buttons = ['random', 'night', 'rush', 'manual'];
            buttons.forEach(btn => {
                const btnEl = document.getElementById(`btn-scn-${btn}`);
                if (btn === type.toLowerCase()) {
                    btnEl.classList.add('active');
                } else {
                    btnEl.classList.remove('active');
                }
            });
            
            const strategyName = document.getElementById('active-scenario-name');
            strategyName.textContent = type === 'RANDOM' ? 'RANDOM TRAFFIC' : type === 'NIGHT' ? 'QUIET NIGHT' : type === 'RUSH' ? 'RUSH HOUR' : 'MANUAL OVERRIDE';
            strategyName.className = type === 'RANDOM' ? 'text-green' : type === 'NIGHT' ? 'text-blue' : type === 'RUSH' ? 'text-red' : 'text-yellow';
            
            refreshLogs();
        }
    } catch (err) {
        console.error('Error setting scenario:', err);
    }
}

// Override Modal Controls
function openOverrideModal(laneName) {
    activeOverrideLane = laneName;
    document.getElementById('modal-lane-name').textContent = laneName;

    // Prefill modal values using the currentState
    if (currentState) {
        const lane = currentState.lanes.find(l => l.name === laneName);
        if (lane) {
            document.getElementById('input-vehicles').value = lane.vehicles;
            document.getElementById('val-vehicles').textContent = lane.vehicles;
            
            document.getElementById('input-pedestrians').value = lane.pedestrians;
            document.getElementById('val-pedestrians').textContent = lane.pedestrians;
            
            document.getElementById('input-emergency').checked = lane.emergency === 1;
        }
    }

    const modal = document.getElementById('override-modal');
    modal.classList.add('open');
}

function closeOverrideModal(event) {
    const modal = document.getElementById('override-modal');
    modal.classList.remove('open');
    activeOverrideLane = null;
}

function updateRangeVal(type) {
    const val = document.getElementById(`input-${type}`).value;
    document.getElementById(`val-${type}`).textContent = val;
}

// Submit overrides
async function submitOverride() {
    if (!activeOverrideLane) return;

    const vehicles = document.getElementById('input-vehicles').value;
    const pedestrians = document.getElementById('input-pedestrians').value;
    const emergency = document.getElementById('input-emergency').checked ? 1 : 0;

    try {
        // UPDATE <lane> <vehicles> <pedestrians> <emergency>
        // sensor_sim automatically enters SCN_MANUAL when it receives UPDATE,
        // so no second SCENARIO MANUAL command is needed (and sending one would
        // race with the first command and potentially overwrite it).
        const response = await fetch('/api/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ command: `UPDATE ${activeOverrideLane} ${vehicles} ${pedestrians} ${emergency}` })
        });

        if (response.ok) {
            setScenarioButtonActive('manual');
            closeOverrideModal();
            refreshLogs();
        }
    } catch (err) {
        console.error('Error submitting override:', err);
    }
}

function setScenarioButtonActive(type) {
    const buttons = ['random', 'night', 'rush', 'manual'];
    buttons.forEach(btn => {
        const btnEl = document.getElementById(`btn-scn-${btn}`);
        if (btn === type) {
            btnEl.classList.add('active');
        } else {
            btnEl.classList.remove('active');
        }
    });
}

// Read and display traffic logs
async function refreshLogs() {
    try {
        const response = await fetch('/api/logs');
        if (!response.ok) return;
        
        const data = await response.json();
        const logTerminal = document.getElementById('log-terminal');
        
        // Store scroll position
        const isScrolledToBottom = logTerminal.scrollHeight - logTerminal.clientHeight <= logTerminal.scrollTop + 10;
        
        logTerminal.innerHTML = '';
        
        data.logs.forEach(line => {
            const lineEl = document.createElement('div');
            lineEl.className = 'terminal-line';
            lineEl.textContent = line;
            
            // Format log highlighting color
            if (line.includes('[EMERGENCY]')) {
                lineEl.className += ' text-red';
            } else if (line.includes('[NIGHT]')) {
                lineEl.className += ' text-blue';
            } else if (line.includes('[SYSTEM]') && line.includes('GREEN')) {
                lineEl.className += ' text-green';
            } else if (line.includes('[STATS]')) {
                lineEl.style.opacity = '0.6';
            }
            
            logTerminal.appendChild(lineEl);
        });

        // Auto scroll if user was at the bottom
        if (isScrolledToBottom) {
            logTerminal.scrollTop = logTerminal.scrollHeight;
        }
    } catch (err) {
        console.error('Error fetching logs:', err);
    }
}
