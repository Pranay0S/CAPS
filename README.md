# CAPS IV Pump Simulator

A Python-based virtual IV pump simulator for the ESE 534 Cyber-Physical Systems project.

The application simulates a simplified infusion pump with:

- adjustable target flow rate
- simulated flow and pressure measurements
- infused volume tracking
- live sensor visualization
- CSV data logging
- pump state monitoring
- simulated sensor attack injection
- a foundation for future anomaly detection and TinyML integration

The project is intended for **educational and research purposes only**. It is not designed to control or interact with a real medical device.

---

## Project Overview

The simulator is divided into two main components:

```text
Frontend
    |
    v
Backend
    |
    +--> Sensor Simulation
    |
    +--> CSV Logging
    |
    +--> Attack Simulation
```

The frontend provides the graphical interface using Tkinter.

The backend manages:

- pump state
- sensor values
- attack injection
- volume calculations
- CSV data collection

The goal is to create a controlled cyber-physical system environment where normal and abnormal sensor behavior can be generated and later analyzed using an anomaly-detection model.

---

## Project Structure

```text
CAPS/
│
├── Backend/
│   ├── __init__.py
│   └── pump_backend.py
│
├── Frontend/
│   ├── __init__.py
│   └── main.py
│
├── docs/
│   └── iv_pump_system.md
│
├── run.py
├── README.md
└── IVP_Data.csv
```

### `Backend/pump_backend.py`

Contains the simulated pump logic, including:

- flow generation
- pressure generation
- pump state
- volume calculation
- CSV logging
- spike attack simulation
- drift attack simulation
- replay attack simulation

### `Frontend/main.py`

Contains the graphical user interface, including:

- live sensor graph
- start and stop controls
- flow-rate input
- alarm display
- system state display
- settings page
- attack injection controls

### `docs/`

Contains additional technical documentation describing the simulated IV pump, dashboard behavior, expected operating scenarios, and fault conditions.

### `run.py`

Main entry point for the application.

---

# Requirements

The project requires:

- Python 3.10 or newer
- Tkinter
- Matplotlib

Most Python installations already include Tkinter.

---

# Installation

## 1. Clone the Repository

```bash
git clone <repository-url>
```

Move into the project folder:

```bash
cd CAPS
```

---

## 2. Check Python Installation

Verify that Python is installed:

```bash
python --version
```

On some systems:

```bash
python3 --version
```

The project is recommended to run with Python 3.10 or newer.

---

## 3. Install Dependencies

Install Matplotlib:

```bash
pip install matplotlib
```

If `pip` is associated with a different Python installation, use:

```bash
python -m pip install matplotlib
```

or:

```bash
python3 -m pip install matplotlib
```

---

## 4. Tkinter

Tkinter is included with most standard Windows Python installations.

You can test whether Tkinter is installed with:

```bash
python -m tkinter
```

A small Tkinter test window should open.

If it does, no additional installation is required.

---

# Running the Simulator

From the top-level `CAPS` directory, run:

```bash
python run.py
```

On some systems:

```bash
python3 run.py
```

Do not normally run `Frontend/main.py` or `Backend/pump_backend.py` directly.

The application should be launched through:

```text
run.py
```

---

# Application Startup

When the program launches, the main dashboard displays:

- pump controls
- alarm status
- live sensor graph
- target flow rate
- actual flow rate
- pressure
- volume infused
- current pump state

The pump initially starts in:

```text
STOPPED
```

Enter a target flow rate and press:

```text
START
```

to begin the simulation.

Press:

```text
STOP
```

to stop the simulated pump.

---

# Settings and Attack Simulation

The settings icon opens the attack simulation page.

The current simulator supports:

### Spike

Creates a sudden abnormal increase in sensor measurements.

### Drift

Gradually shifts sensor measurements away from their expected values.

### Replay

Uses previously collected sensor measurements in place of current data.

These simulations are intended to generate abnormal time-series data for future cybersecurity and anomaly-detection experiments.

---

# Data Collection

The simulator automatically creates:

```text
IVP_Data.csv
```

The CSV contains values such as:

```text
timestamp
target_flow_rate
actual_flow_rate
pressure
volume_infused
state
alarm
attack
```

Example:

```csv
timestamp,target_flow_rate,actual_flow_rate,pressure,volume_infused,state,alarm,attack
2026-10-04T14:30:01,100.0,99.62,10.21,0.0138,RUNNING,NONE,NONE
2026-10-04T14:30:02,100.0,100.34,9.88,0.0277,RUNNING,NONE,NONE
2026-10-04T14:30:03,100.0,172.50,28.34,0.0516,RUNNING,NONE,SPIKE
```

The collected data can later be used for:

- signal analysis
- attack classification
- anomaly detection
- machine-learning training
- TinyML experiments

---

# Current Architecture

```text
User
 |
 v
Tkinter Dashboard
 |
 v
IVPumpSimulator
 |
 +---------------------+
 |                     |
 v                     v
Sensor Simulation    CSV Logger
 |
 v
Attack Injection
 |
 v
Future Detection Layer
```

A future version of the project is intended to add an anomaly-detection layer between the simulated sensors and trusted pump logic.

---

# Future Development

Planned improvements include:

- faster sensor sampling
- independent graph refresh rate
- anomaly scoring
- rule-based detection
- TinyML model integration
- attack detection alerts
- additional simulated faults
- improved graph controls
- selectable graph signals
- safe-state behavior

---

# Troubleshooting

## `ModuleNotFoundError`

Make sure you are running the program from the top-level project directory:

```bash
python run.py
```

The folder structure should remain:

```text
CAPS/
├── Backend/
├── Frontend/
└── run.py
```

---

## Matplotlib Not Found

If you receive:

```text
ModuleNotFoundError: No module named 'matplotlib'
```

install it with:

```bash
python -m pip install matplotlib
```

---

## Tkinter Not Found

Test Tkinter with:

```bash
python -m tkinter
```

If a window does not open, your Python installation may not include Tkinter.

---

## CSV File Not Updating

Check that:

- the project folder is writable
- `IVP_Data.csv` is not locked by another program
- the simulator is running
- the pump has been started

---

# Safety Notice

This project is a software simulation created for coursework and research.

It is **not a medical device** and should not be used to:

- control a real infusion pump
- deliver medication
- determine medical dosing
- monitor patients
- make clinical decisions

All sensor values, alarms, faults, and attack behaviors are simulated.

---

# Course

**ESE 534 — Cyber-Physical Systems**

Project focus:

**Cyber-physical security and anomaly detection in a simulated IV pump system.**