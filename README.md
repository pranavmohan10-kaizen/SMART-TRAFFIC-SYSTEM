# 🚦 AI-Based Smart Traffic Management System

A self-contained, intelligent traffic controller written in **pure C (C11)**.

This project simulates a multi-lane, real-world traffic intersection. Instead of relying on static, pre-programmed timers, it uses **Rule-Based AI (Decision Trees)** to dynamically allocate green lights based on live vehicle density, pedestrian foot traffic, and emergency vehicle detection.

There are no heavy Machine Learning models or external C++ libraries here—just highly optimized, pure C programming utilizing standard algorithms like Bubble Sort and deterministic conditional logic to emulate AI behavior.

---

## 🛠️ Tech Stack

* **Language:** Pure C (C11 Standard)
* **Build System:** Make / `Makefile`
* **UI/Frontend:** ANSI Escape Codes (for a live, color-coded terminal dashboard)
* **Data Handling:** Atomic File I/O (simulated hardware sensor streaming)

---

## 🧠 System Architecture

The project is split into **two completely independent executables** that run simultaneously. They communicate with each other through a shared text file, perfectly mimicking how an actual traffic controller receives data from inductive road loops and traffic cameras.

```text
   ┌────────────────┐     live_sensor_data.txt      ┌──────────────────┐
   │   sensor_sim   │ ───────────────────────────▶  │  traffic_engine  │
   │  (data source) │   (atomic overwrite, 6 lanes) │   (controller)   │
   └────────────────┘                               └──────────────────┘
```

### Atomic Overwrite

To prevent the main engine from crashing by reading a half-written file, the simulator writes data to a hidden temporary file first and then instantly renames it. This guarantees the engine always reads a perfectly complete sensor snapshot.

### 📂 File Breakdown

#### 1. `traffic_system.h`

The central blueprint. It holds all the shared data structures (`structs`), logic thresholds (what counts as "Heavy" traffic), and UI color definitions.

#### 2. `sensor_sim.c`

The hardware emulator. It uses random number generation to simulate live data for 6 lanes (vehicles, pedestrians, ambulances) and streams it to the shared text file. It also injects specific scenarios (such as Rush Hour or Quiet Night) to test the AI.

#### 3. `traffic_engine.c`

The central brain. It continuously reads the sensor file, runs the data through the AI Decision Tree, calculates signal timings, and paints the live dashboard to the terminal.

#### 4. `Makefile`

The build script that compiles the entire project with a single command.

---

## 🌳 The AI Decision Tree Logic

Every second, the engine evaluates the live data through a top-down decision tree to determine the safest and most efficient traffic flow.

### 🚨 Branch 1: Emergency Override (Highest Priority)

**Condition:** Is an ambulance or fire truck detected?

**Action:** Instant GREEN for that lane. All other lanes go RED, and all pedestrian crosswalks are HELD until the priority vehicle clears the intersection.

---

### 🌙 Branch 2: Night Mode (Safety First)

**Condition:** Are ALL lanes reporting LOW traffic (<15 cars) and FEW pedestrians (<5)?

**Action:** Activates a Rapid Cycle. Green times are compressed to 3–5 seconds to keep the few cars moving. Empty lanes are skipped entirely.

**Design Note:** Blinking yellow lights were intentionally avoided to reduce the risk of late-night T-bone collisions.

---

### 📊 Branch 3: Normal Adaptive Control

**Condition:** Standard traffic conditions.

**Action:** The system classifies lanes into density tiers (Low, Medium, Severe). It then uses a **Bubble Sort** algorithm to rank the lanes. The worst bottleneck gets served first, while pedestrian WALK signals are carefully coordinated to trigger only when vehicles have a RED light.

---

## 🚀 How to Run the Project

To run this project locally, you need a C compiler (such as GCC) and `make` installed on your system.

### Step 1: Clone and Compile

Open a terminal inside the project directory and run:

```sh
make
```

This generates two executable files:

* `sensor_sim`
* `traffic_engine`

---

### Step 2: Prepare the Windows Terminal (Important)

On Windows, the default terminal may not correctly display the dashboard borders and special characters. If you see strange symbols such as `Γöé`, switch the terminal to UTF-8 mode by running:

```cmd
chcp 65001
```

---

### Step 3: Launch the System

The architecture requires two separate processes running simultaneously.

Open **two terminal windows** in the project folder.

#### Terminal 1 — Start the Sensor Simulator

**Windows**

```sh
.\sensor_sim
```

**Linux / macOS**

```sh
./sensor_sim
```

---

#### Terminal 2 — Start the Traffic Controller

**Windows**

```sh
.\traffic_engine
```

**Linux / macOS**

```sh
./traffic_engine
```

> If you are on Windows, remember to run `chcp 65001` in this terminal as well.

---

### Stopping the System

Press:

```text
Ctrl + C
```

in both terminals.

The project also generates a persistent audit log:

```text
traffic_log.txt
```

This file contains a detailed record of the AI's traffic-management decisions during runtime.

---

## 🎯 Key Features

* Adaptive traffic signal control
* Emergency vehicle prioritization
* Dynamic pedestrian crossing management
* Night-mode optimization
* Atomic file-based inter-process communication
* Live color-coded dashboard
* Pure C11 implementation with no external dependencies
* Multi-process architecture using independent executables
* Deterministic rule-based AI using decision trees


