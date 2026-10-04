# IV Pump Simulator System Documentation

## 1. Overview

An **infusion pump**, commonly called an **IV pump**, is a medical device used to deliver fluids into a patient's body at a controlled rate. These fluids may include medications, nutrients, saline, antibiotics, pain medication, insulin, or other therapies.

Unlike a gravity-fed IV, an infusion pump allows a clinician to program a specific delivery rate and duration. The pump then regulates the delivery of fluid according to those programmed settings.

Real infusion pumps may monitor variables such as:

- programmed flow rate
- estimated or measured fluid delivery
- pressure within the infusion line
- remaining volume
- occlusions or restrictions in tubing
- air-in-line conditions
- battery and hardware status

The purpose of this project is **not to reproduce a clinical infusion pump**. Instead, it creates a software simulation that behaves like a simplified cyber-physical system.

The simulator provides:

- a virtual pump state
- simulated flow and pressure measurements
- adjustable pump settings
- real-time sensor visualization
- CSV data collection
- simulated abnormal sensor behavior
- a platform for future anomaly-detection and TinyML experiments

The simulator is intended for research and educational use only.

---

# 2. System Architecture

The project is divided into a frontend and backend.

```text
                User Interface
                     |
                     v
             Frontend/main.py
                     |
                     v
          Backend/pump_backend.py
                     |
           +---------+---------+
           |                   |
           v                   v
    Pump Simulation        CSV Logger
           |
           v
   Simulated Sensor Data
           |
           v
     Future Detection
          Layer
```

The frontend is responsible for displaying information and accepting user input.

The backend is responsible for maintaining the simulated pump state and generating sensor measurements.

This separation makes it possible to modify the underlying pump behavior without redesigning the user interface.

---

# 3. Simulated Measurements

The current simulator tracks three primary measurements.

## 3.1 Flow Rate

Flow rate represents the rate at which fluid is being delivered.

The target flow rate is selected by the user.

Example:

```text
Target Flow Rate: 100 mL/hr
Actual Flow Rate: 99.4 mL/hr
```

The simulator intentionally introduces small amounts of random variation into the actual flow measurement.

This represents normal sensor variation and prevents the simulated data from being perfectly constant.

Under normal operation:

```text
Actual Flow Rate ≈ Target Flow Rate
```

A small difference between the two values is expected.

---

## 3.2 Pressure

Pressure represents the simulated pressure within the infusion line.

Pressure can be useful for detecting conditions such as:

- blocked tubing
- restricted flow
- occlusion
- abnormal pump behavior

The current project uses simulated pressure values rather than attempting to reproduce a specific commercial pump's pressure units or calibration.

Normal pressure values should remain within a relatively stable range.

Abnormal conditions may cause rapid or gradual pressure changes.

---

## 3.3 Volume Infused

Volume infused represents the approximate amount of fluid delivered since the pump began operating.

The simulator calculates volume from the measured flow rate.

Conceptually:

```text
Volume = Flow Rate × Time
```

Because the flow rate is expressed in mL/hr, it is first converted into mL/s.

```text
flow_per_second = flow_rate / 3600
```

The simulator then adds the delivered volume during each update cycle.

---

# 4. Main Dashboard

The primary dashboard provides the operator with a real-time view of the simulated system.

The intended layout is:

```text
+------------------------------------------------------------------+
| IV PUMP SIMULATOR | Cyber-Physical Security & Sensor Monitoring  |
+------------------------------------------------------------------+

   START        STOP                         ALARM STATUS: NORMAL


   +--------------------------------------+  +--------------------+
   |                                      |  | TARGET FLOW RATE   |
   |         LIVE SENSOR GRAPH            |  | 100 mL/hr          |
   |                                      |  |                    |
   |                                      |  | ACTUAL FLOW        |
   |                                      |  | 99.4 mL/hr         |
   |                                      |  |                    |
   |                                      |  | PRESSURE           |
   |                                      |  | 10.1               |
   |                                      |  |                    |
   |                                      |  | VOLUME             |
   +--------------------------------------+  | 4.24 mL            |
                                             +--------------------+

                     SYSTEM STATE

                       RUNNING
```

---

# 5. Dashboard Components

## 5.1 Header

The top banner identifies the application.

Example:

```text
IV PUMP SIMULATOR | Cyber-Physical Security & Sensor Monitoring
```

The header should remain visible while using the main dashboard.

A settings icon should appear on the right side of the header.

Selecting the settings icon opens the attack simulation page.

---

# 5.2 Start Button

The **START** button changes the pump state from:

```text
STOPPED
```

to:

```text
RUNNING
```

Before starting, the simulator should confirm that the programmed target flow rate is valid.

### Normal behavior

When START is pressed:

1. The pump enters the `RUNNING` state.
2. Flow measurements begin approaching the selected target.
3. Pressure measurements begin updating.
4. Volume infused begins increasing.
5. Sensor values continue being written to the CSV file.
6. The live graph begins displaying the measurements.

### Possible failure cases

The pump should not start if:

- the target flow field is empty
- the value is not numeric
- the flow rate is zero
- the flow rate is negative

The interface should display an error instead of allowing invalid input.

---

# 5.3 Stop Button

The **STOP** button places the simulated pump into the `STOPPED` state.

### Expected behavior

When STOP is pressed:

```text
State = STOPPED
Actual Flow Rate = 0
```

The previously infused volume should remain stored rather than returning to zero.

Pressure may continue displaying a simulated baseline reading.

CSV logging may continue so that the transition between running and stopped states is captured.

---

# 5.4 Alarm Status

The alarm display communicates whether the simulated system has identified a fault condition.

Example normal display:

```text
● NORMAL
```

Example abnormal display:

```text
● ALARM
```

or:

```text
● SENSOR ANOMALY
```

Normal status should be visually distinct from an abnormal status.

For example:

```text
Green = normal
Red   = alarm
```

The alarm field should eventually represent the output of the detection system rather than simply indicate that an attack button was pressed.

This distinction is important.

The attack simulator represents what is happening to the sensor data.

The alarm system represents whether the defense system successfully detected the abnormal behavior.

---

# 5.5 Target Flow Rate

The target flow rate represents the programmed pump setting.

Example:

```text
100 mL/hr
```

The user should be able to modify this value.

A **SET** button can be used to apply a new target.

### Normal scenario

If the operator changes:

```text
100 mL/hr
```

to:

```text
120 mL/hr
```

the actual simulated flow should gradually or immediately move toward the new value depending on the simulation model.

### Possible future improvement

Instead of changing immediately, the simulator could model pump response time.

For example:

```text
100
104
109
114
118
120
```

This would create more realistic time-series data.

---

# 5.6 Actual Flow Rate

Actual flow represents the simulated sensor measurement.

During normal operation:

```text
Target = 100 mL/hr
Actual ≈ 98.5 - 101.5 mL/hr
```

Small random variations are intentionally included.

Large changes may represent:

- a simulated sensor attack
- an occlusion
- incorrect data
- a hardware fault

---

# 5.7 Pressure

The pressure display represents a second sensor channel.

Under normal operation the value should remain relatively stable.

The pressure signal is useful because comparing multiple sensor channels can make anomaly detection more reliable.

For example:

```text
Flow suddenly decreases
Pressure suddenly increases
```

could be consistent with an occlusion-type condition.

However:

```text
Flow suddenly increases
Pressure remains completely unchanged
```

may indicate corrupted or manipulated sensor data depending on the model.

---

# 5.8 Volume Infused

Volume infused accumulates while the pump is running.

Example:

```text
0.000 mL
0.014 mL
0.028 mL
0.042 mL
...
```

When the pump stops, the volume should stop increasing.

The value should not reset unless a separate reset operation is explicitly implemented.

---

# 5.9 Live Sensor Graph

The live graph visualizes changes in sensor measurements over time.

The graph is intended to make abnormal behavior easy to identify visually.

Possible signals include:

- flow rate
- pressure
- volume
- future anomaly score

The graph should maintain only a recent time window instead of displaying every measurement recorded since the program started.

For example:

```text
Last 50 samples
```

or:

```text
Last 30 seconds
```

This keeps the graph readable and reduces rendering overhead.

A future version may allow the operator to choose which signal is shown:

```text
Display:
[ Flow Rate v ]
```

with options such as:

```text
Flow Rate
Pressure
Volume
Anomaly Score
```

---

# 5.10 System State

The state display appears below the graph.

Possible states currently include:

```text
STOPPED
RUNNING
```

Future states may include:

```text
ALARM
FAULT
PAUSED
SAFE MODE
```

The state represents the operating condition of the simulated pump.

The state should be treated separately from the alarm.

For example:

```text
State: RUNNING
Alarm: NORMAL
```

is normal operation.

A future defense system might produce:

```text
State: SAFE MODE
Alarm: SENSOR ATTACK DETECTED
```

---

# 6. Settings / Attack Simulation Page

The settings icon opens a separate page used for cybersecurity testing.

This page includes simulated attacks that modify sensor data.

The current planned attacks are:

- spike
- drift
- replay

These attacks should only affect the software simulation and are intended to generate abnormal data for detection experiments.

---

# 7. Spike Injection

A spike represents a sudden and unusually large change in a sensor measurement.

Example normal signal:

```text
99.5
100.2
99.8
100.4
```

Spike:

```text
174.2
```

then:

```text
100.1
99.7
```

Graphically:

```text
          /\
---------/  \---------
```

### Purpose

Spike attacks are useful for testing whether a detection algorithm can identify obvious short-duration anomalies.

### Ideal detector response

A detection system should recognize that the rapid change is inconsistent with recent measurements.

Possible result:

```text
ALARM STATUS:
ANOMALY DETECTED
```

### Possible detection problem

A poorly designed detector may mistake legitimate transient noise for a malicious spike.

This creates a **false positive**.

---

# 8. Drift Injection

Drift represents a gradual change in sensor data.

Example:

```text
100.0
100.5
101.0
101.6
102.2
103.0
104.1
```

Unlike a spike, no individual measurement may appear extremely abnormal.

Graphically:

```text
              /
            /
          /
        /
------/
```

### Purpose

Drift is more difficult to detect because measurements change slowly.

Simple threshold systems may fail if every individual sample remains inside the allowed range.

### Ideal detector response

The detection system should recognize that the signal's long-term behavior has changed even if each individual reading appears plausible.

### Expected challenge

Drift detection may require:

- historical measurements
- moving averages
- rate-of-change calculations
- sequence-based models
- machine-learning models

---

# 9. Replay Injection

A replay attack uses previously valid sensor measurements and presents them as if they were current measurements.

Example:

The pump previously recorded:

```text
99.8
100.1
100.0
99.7
```

During a replay attack, these old values may be repeatedly supplied even though the real simulated system has changed.

### Why replay attacks are interesting

The measurements themselves may appear completely normal.

Unlike a spike:

```text
200 mL/hr
```

a replay sample might be:

```text
99.9 mL/hr
```

which appears reasonable.

The abnormal behavior is related to the timing and repetition of the measurements rather than their magnitude.

### Ideal detector response

A stronger detection system may recognize:

- repeated sequences
- lack of expected variation
- inconsistent relationships between sensors
- timestamps that do not match
- disagreement between physical state and reported sensor values

Replay detection is therefore useful for demonstrating why simple threshold-based security is often insufficient.

---

# 10. Normal Operating Scenario

A normal test may follow this sequence:

```text
1. Application starts.
2. State displays STOPPED.
3. Operator enters 100 mL/hr.
4. Operator presses START.
5. State changes to RUNNING.
6. Actual flow stabilizes near 100 mL/hr.
7. Pressure remains near baseline.
8. Volume gradually increases.
9. Graph displays small natural fluctuations.
10. Alarm remains NORMAL.
```

The CSV should contain values similar to:

```text
timestamp,target_flow_rate,actual_flow_rate,pressure,volume_infused,state,alarm,attack
12:00:00,100,99.4,10.2,0.0138,RUNNING,NONE,NONE
12:00:01,100,100.5,9.9,0.0278,RUNNING,NONE,NONE
12:00:02,100,99.8,10.1,0.0417,RUNNING,NONE,NONE
```

---

# 11. Example Attack Scenario

A simulated spike test may behave as follows:

```text
Target Flow: 100 mL/hr

Normal:
99.8
100.3
99.5
100.1

Attack begins:
172.4
181.7
164.8

Attack ends:
100.4
99.9
```

The graph should clearly show the abnormal region.

The CSV should record the attack label:

```text
SPIKE
```

This allows the collected dataset to later be used for training and testing anomaly-detection models.

---

# 12. Expected Failure Modes

Several software and simulation failures should be considered during development.

## Invalid User Input

Example:

```text
Target Flow = abc
```

Expected response:

```text
Error: Enter a valid positive flow rate.
```

The application should remain running.

---

## Backend / Frontend Key Mismatch

The frontend expects dictionary fields such as:

```python
actual_flow_rate
pressure
volume_infused
state
alarm
```

If the backend changes these names without updating the frontend, Python may generate a `KeyError`.

Frontend and backend interfaces should therefore remain consistent.

---

## CSV File Problems

Possible issues include:

- CSV file is open in another application
- directory is read-only
- storage is unavailable
- file path is invalid

Logging errors should eventually be handled so that a CSV failure does not necessarily crash the entire simulator.

---

## Graph Rendering Delay

The graph may appear slow or unresponsive if it is completely redrawn too frequently.

Matplotlib is designed primarily for scientific plotting rather than high-speed GUI visualization.

Repeated operations such as:

```python
axis.relim()
axis.autoscale_view()
canvas.draw()
```

can become expensive when performed many times per second.

The graphical refresh rate should therefore be separated from the sensor sampling rate.

For example:

```text
Sensor sampling: 10 Hz
CSV logging:     10 Hz
Graph refresh:    5 Hz
```

The backend can collect sensor measurements faster than the interface draws them.

---

# 13. Sensor Sampling vs Display Refresh

It is important to separate three concepts:

```text
Sensor sampling rate
Data logging rate
Display refresh rate
```

They do not need to be identical.

A real system may collect sensor measurements rapidly while updating the visual interface more slowly.

For this simulator, a reasonable architecture would be:

```text
Sensor Simulation
        |
        | 100 ms
        v
     10 samples/s
        |
        +-----------------> CSV
        |
        v
   graph refresh
      200 ms
     5 frames/s
```

This provides enough data for anomaly-detection experiments without forcing Matplotlib to redraw unnecessarily often.

---

# 14. Future Detection Layer

The final project architecture is expected to include a detection layer between sensor generation and trusted pump logic.

```text
Simulated Physical System
          |
          v
      Sensor Data
          |
          v
+-----------------------+
|  Anomaly Detection    |
|                       |
| Rules / ML / TinyML   |
+-----------+-----------+
            |
      +-----+-----+
      |           |
    SAFE        UNSAFE
      |           |
      v           v
 Pump Logic     Alarm /
 Continues      Safe State
```

The detector may analyze combinations of:

```text
flow
pressure
volume
rate of change
historical measurements
relationships between sensors
```

The spike, drift, and replay simulations provide different anomaly classes that can be used to evaluate the detector.

---

# 15. Project Scope

This project is an educational cyber-physical system simulation.

It does **not** implement:

- clinical medication delivery
- validated medical-device control
- patient-specific dosing
- medical safety limits
- certified alarm logic
- real infusion hardware

It should not be used to control or interface with a real medical device.

The project instead provides a controlled environment for studying:

- embedded-system concepts
- sensor data
- cyber-physical attacks
- anomaly detection
- state-machine behavior
- data collection
- machine-learning-based defenses