import csv
import os
import random
from datetime import datetime
from collections import deque


class IVPumpSimulator:

    def __init__(self, csv_file="IVP_Data.csv"):

        self.csv_file = csv_file

        # -------------------------------
        # Pump state
        # -------------------------------

        self.state = "STOPPED"
        self.alarm = "NONE"

        # -------------------------------
        # Pump measurements
        # -------------------------------

        self.target_flow_rate = 100.0
        self.actual_flow_rate = 0.0
        self.pressure = 10.0
        self.volume_infused = 0.0

        # -------------------------------
        # Attack simulation
        # -------------------------------

        self.attack = "NONE"

        self.spike_counter = 0
        self.drift_offset = 0.0

        # Stores previous sensor readings
        self.sensor_history = deque(maxlen=50)

        self._create_csv()

    # ==================================================
    # CSV
    # ==================================================

    def _create_csv(self):

        if os.path.exists(self.csv_file):
            return

        with open(self.csv_file, "w", newline="") as file:

            writer = csv.writer(file)

            writer.writerow([
                "timestamp",
                "target_flow_rate",
                "actual_flow_rate",
                "pressure",
                "volume_infused",
                "state",
                "alarm",
                "attack"
            ])

    def _log_data(self):

        with open(self.csv_file, "a", newline="") as file:

            writer = csv.writer(file)

            writer.writerow([
                datetime.now().isoformat(timespec="seconds"),
                round(self.target_flow_rate, 2),
                round(self.actual_flow_rate, 2),
                round(self.pressure, 2),
                round(self.volume_infused, 4),
                self.state,
                self.alarm,
                self.attack
            ])

    # ==================================================
    # PUMP CONTROL
    # ==================================================

    def start(self):

        self.state = "RUNNING"
        self.alarm = "NONE"

    def stop(self):

        self.state = "STOPPED"

        self.actual_flow_rate = 0.0

    def set_flow_rate(self, flow_rate):

        self.target_flow_rate = float(flow_rate)

    # ==================================================
    # ATTACK INJECTION
    # ==================================================

    def inject_spike(self):

        self.attack = "SPIKE"

        # Spike lasts several samples
        self.spike_counter = 6

    def inject_drift(self):

        if self.attack == "DRIFT":

            # Pressing again removes drift
            self.attack = "NONE"
            self.drift_offset = 0.0

        else:

            self.attack = "DRIFT"
            self.drift_offset = 0.0

    def inject_replay(self):

        if len(self.sensor_history) < 5:
            return

        self.attack = "REPLAY"

    def clear_attack(self):

        self.attack = "NONE"

        self.spike_counter = 0
        self.drift_offset = 0.0

    # ==================================================
    # SENSOR SIMULATION
    # ==================================================

    def update(self, dt):

        if self.state == "RUNNING":

            # ------------------------------------------
            # NORMAL SENSOR VALUES
            # ------------------------------------------

            normal_flow = (
                self.target_flow_rate
                + random.uniform(-1.5, 1.5)
            )

            normal_pressure = random.uniform(
                8.0,
                12.0
            )

            # ------------------------------------------
            # SPIKE ATTACK
            # ------------------------------------------

            if self.attack == "SPIKE":

                self.actual_flow_rate = (
                    normal_flow
                    + random.uniform(50, 90)
                )

                self.pressure = (
                    normal_pressure
                    + random.uniform(15, 30)
                )

                self.spike_counter -= 1

                if self.spike_counter <= 0:
                    self.attack = "NONE"

            # ------------------------------------------
            # DRIFT ATTACK
            # ------------------------------------------

            elif self.attack == "DRIFT":

                self.drift_offset += 0.4

                self.actual_flow_rate = (
                    normal_flow
                    + self.drift_offset
                )

                self.pressure = (
                    normal_pressure
                    + self.drift_offset * 0.15
                )

            # ------------------------------------------
            # REPLAY ATTACK
            # ------------------------------------------

            elif self.attack == "REPLAY":

                old_data = self.sensor_history[0]

                self.actual_flow_rate = old_data[0]

                self.pressure = old_data[1]

            # ------------------------------------------
            # NORMAL OPERATION
            # ------------------------------------------

            else:

                self.actual_flow_rate = normal_flow
                self.pressure = normal_pressure

            # ------------------------------------------
            # Volume calculation
            # ------------------------------------------

            flow_per_second = (
                self.actual_flow_rate / 3600.0
            )

            self.volume_infused += (
                flow_per_second * dt
            )

            # Save current reading
            self.sensor_history.append(
                (
                    self.actual_flow_rate,
                    self.pressure
                )
            )

        else:

            self.actual_flow_rate = 0.0

            self.pressure = random.uniform(
                8.0,
                12.0
            )

        self._log_data()

    # ==================================================
    # FRONTEND DATA
    # ==================================================

    def get_sensor_data(self):

        return {
            "target_flow_rate": self.target_flow_rate,
            "actual_flow_rate": self.actual_flow_rate,
            "pressure": self.pressure,
            "volume_infused": self.volume_infused,
            "state": self.state,
            "alarm": self.alarm,
            "attack": self.attack
        }