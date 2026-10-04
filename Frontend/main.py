import tkinter as tk
from tkinter import messagebox

from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

from Backend.pump_backend import IVPumpSimulator


class PumpGUI:

    def __init__(self, root):

        self.root = root

        self.root.title("IV Pump Simulator")

        self.root.geometry("1200x760")

        # Allow resize
        self.root.minsize(1000, 650)

        # ==================================================
        # COLORS
        # ==================================================

        self.BG = "#0B1120"
        self.HEADER = "#111827"
        self.PANEL = "#151F32"
        self.CARD = "#1E293B"

        self.TEXT = "#F8FAFC"
        self.MUTED = "#94A3B8"

        self.BLUE = "#3B82F6"
        self.GREEN = "#22C55E"
        self.RED = "#EF4444"
        self.YELLOW = "#F59E0B"

        self.root.configure(bg=self.BG)

        # ==================================================
        # BACKEND
        # ==================================================

        self.pump = IVPumpSimulator()

        self.update_interval = 500

        # ==================================================
        # GRAPH DATA
        # ==================================================

        self.time_data = []
        self.flow_data = []
        self.pressure_data = []

        self.sample_number = 0

        # ==================================================
        # MAIN PAGE CONTAINER
        # ==================================================

        self.container = tk.Frame(
            self.root,
            bg=self.BG
        )

        self.container.pack(
            fill="both",
            expand=True
        )

        self.show_dashboard()

        self.update_gui()

    # ==================================================
    # CLEAR PAGE
    # ==================================================

    def clear_page(self):

        for widget in self.container.winfo_children():
            widget.destroy()

    # ==================================================
    # HEADER
    # ==================================================

    def build_header(self, settings_button=True):

        header = tk.Frame(
            self.container,
            bg=self.HEADER,
            height=75
        )

        header.pack(
            fill="x"
        )

        header.pack_propagate(False)

        title_frame = tk.Frame(
            header,
            bg=self.HEADER
        )

        title_frame.pack(
            side="left",
            padx=30
        )

        tk.Label(
            title_frame,
            text="IV PUMP SIMULATOR",
            font=("Segoe UI", 18, "bold"),
            fg=self.TEXT,
            bg=self.HEADER
        ).pack(
            side="left"
        )

        tk.Label(
            title_frame,
            text=" | ",
            font=("Segoe UI", 18),
            fg=self.MUTED,
            bg=self.HEADER
        ).pack(
            side="left"
        )

        tk.Label(
            title_frame,
            text="Cyber-Physical Security & Sensor Monitoring",
            font=("Segoe UI", 11),
            fg=self.MUTED,
            bg=self.HEADER
        ).pack(
            side="left"
        )

        # ------------------------------------------
        # SETTINGS / BACK BUTTON
        # ------------------------------------------

        if settings_button:

            tk.Button(
                header,
                text="⚙",
                command=self.show_settings,
                font=("Segoe UI Symbol", 20),
                fg=self.TEXT,
                bg=self.HEADER,
                activebackground=self.CARD,
                activeforeground=self.TEXT,
                relief="flat",
                borderwidth=0,
                cursor="hand2"
            ).pack(
                side="right",
                padx=30
            )

        else:

            tk.Button(
                header,
                text="←  Dashboard",
                command=self.show_dashboard,
                font=("Segoe UI", 11, "bold"),
                fg=self.TEXT,
                bg=self.HEADER,
                activebackground=self.CARD,
                activeforeground=self.TEXT,
                relief="flat",
                borderwidth=0,
                cursor="hand2"
            ).pack(
                side="right",
                padx=30
            )

    # ==================================================
    # DASHBOARD PAGE
    # ==================================================

    def show_dashboard(self):

        self.clear_page()

        self.build_header(
            settings_button=True
        )

        # ==================================================
        # TOP CONTROLS
        # ==================================================

        controls = tk.Frame(
            self.container,
            bg=self.BG
        )

        controls.pack(
            fill="x",
            padx=30,
            pady=(22, 15)
        )

        # START
        tk.Button(
            controls,
            text="▶  START",
            command=self.start_pump,
            font=("Segoe UI", 10, "bold"),
            fg="white",
            bg=self.GREEN,
            activebackground="#16A34A",
            activeforeground="white",
            relief="flat",
            borderwidth=0,
            padx=25,
            pady=11,
            cursor="hand2"
        ).pack(
            side="left",
            padx=(0, 8)
        )

        # STOP
        tk.Button(
            controls,
            text="■  STOP",
            command=self.stop_pump,
            font=("Segoe UI", 10, "bold"),
            fg="white",
            bg=self.RED,
            activebackground="#DC2626",
            activeforeground="white",
            relief="flat",
            borderwidth=0,
            padx=25,
            pady=11,
            cursor="hand2"
        ).pack(
            side="left"
        )

        # ------------------------------------------
        # ALARM
        # ------------------------------------------

        alarm_box = tk.Frame(
            controls,
            bg=self.PANEL
        )

        alarm_box.pack(
            side="right",
            ipadx=18,
            ipady=7
        )

        tk.Label(
            alarm_box,
            text="ALARM STATUS",
            font=("Segoe UI", 8, "bold"),
            fg=self.MUTED,
            bg=self.PANEL
        ).pack(
            side="left",
            padx=(12, 12)
        )

        self.alarm_dot = tk.Label(
            alarm_box,
            text="●",
            font=("Segoe UI", 15),
            fg=self.GREEN,
            bg=self.PANEL
        )

        self.alarm_dot.pack(
            side="left"
        )

        self.alarm_display = tk.Label(
            alarm_box,
            text="NORMAL",
            font=("Segoe UI", 11, "bold"),
            fg=self.TEXT,
            bg=self.PANEL
        )

        self.alarm_display.pack(
            side="left",
            padx=(5, 14)
        )

        # ==================================================
        # GRAPH + RIGHT PANEL
        # ==================================================

        middle = tk.Frame(
            self.container,
            bg=self.BG
        )

        middle.pack(
            fill="both",
            expand=True,
            padx=30
        )

        # ==================================================
        # GRAPH PANEL
        # ==================================================

        graph_panel = tk.Frame(
            middle,
            bg=self.PANEL
        )

        graph_panel.pack(
            side="left",
            fill="both",
            expand=True,
            padx=(0, 12)
        )

        tk.Label(
            graph_panel,
            text="LIVE SENSOR MONITOR",
            font=("Segoe UI", 10, "bold"),
            fg=self.MUTED,
            bg=self.PANEL
        ).pack(
            anchor="w",
            padx=20,
            pady=(17, 0)
        )

        # ------------------------------------------
        # MATPLOTLIB GRAPH
        # ------------------------------------------

        self.figure = Figure(
            figsize=(7, 4),
            dpi=100
        )

        self.figure.patch.set_facecolor(
            self.PANEL
        )

        self.axis = self.figure.add_subplot(111)

        self.axis.set_facecolor(
            self.PANEL
        )

        self.axis.tick_params(
            colors=self.MUTED
        )

        for spine in self.axis.spines.values():
            spine.set_color("#334155")

        self.axis.set_xlabel(
            "Sample",
            color=self.MUTED
        )

        self.axis.set_ylabel(
            "Sensor Value",
            color=self.MUTED
        )

        self.axis.grid(
            True,
            alpha=0.15
        )

        self.flow_line, = self.axis.plot(
            [],
            [],
            label="Flow Rate"
        )

        self.pressure_line, = self.axis.plot(
            [],
            [],
            label="Pressure"
        )

        legend = self.axis.legend(
            loc="upper left"
        )

        self.canvas = FigureCanvasTkAgg(
            self.figure,
            graph_panel
        )

        self.canvas.get_tk_widget().pack(
            fill="both",
            expand=True,
            padx=10,
            pady=10
        )

        # ==================================================
        # RIGHT SIDE VALUES
        # ==================================================

        side_panel = tk.Frame(
            middle,
            bg=self.PANEL,
            width=270
        )

        side_panel.pack(
            side="right",
            fill="y"
        )

        side_panel.pack_propagate(False)

        tk.Label(
            side_panel,
            text="PUMP PARAMETERS",
            font=("Segoe UI", 10, "bold"),
            fg=self.MUTED,
            bg=self.PANEL
        ).pack(
            anchor="w",
            padx=20,
            pady=(20, 18)
        )

        # ------------------------------------------
        # TARGET FLOW
        # ------------------------------------------

        self.make_parameter_input(
            side_panel
        )

        self.flow_display = self.make_value(
            side_panel,
            "ACTUAL FLOW",
            "0.00",
            "mL/hr"
        )

        self.pressure_display = self.make_value(
            side_panel,
            "PRESSURE",
            "0.00",
            "sensor units"
        )

        self.volume_display = self.make_value(
            side_panel,
            "VOLUME INFUSED",
            "0.000",
            "mL"
        )

        # ==================================================
        # STATE BAR
        # ==================================================

        state_panel = tk.Frame(
            self.container,
            bg=self.PANEL
        )

        state_panel.pack(
            fill="x",
            padx=30,
            pady=(15, 25)
        )

        state_inner = tk.Frame(
            state_panel,
            bg=self.PANEL
        )

        state_inner.pack(
            pady=16
        )

        tk.Label(
            state_inner,
            text="SYSTEM STATE",
            font=("Segoe UI", 9, "bold"),
            fg=self.MUTED,
            bg=self.PANEL
        ).pack(
            side="left",
            padx=(0, 20)
        )

        self.state_dot = tk.Label(
            state_inner,
            text="●",
            font=("Segoe UI", 18),
            fg=self.YELLOW,
            bg=self.PANEL
        )

        self.state_dot.pack(
            side="left"
        )

        self.state_display = tk.Label(
            state_inner,
            text="STOPPED",
            font=("Segoe UI", 16, "bold"),
            fg=self.TEXT,
            bg=self.PANEL
        )

        self.state_display.pack(
            side="left",
            padx=8
        )

    # ==================================================
    # PARAMETER INPUT
    # ==================================================

    def make_parameter_input(self, parent):

        frame = tk.Frame(
            parent,
            bg=self.CARD
        )

        frame.pack(
            fill="x",
            padx=15,
            pady=5
        )

        tk.Label(
            frame,
            text="TARGET FLOW RATE",
            font=("Segoe UI", 8, "bold"),
            fg=self.MUTED,
            bg=self.CARD
        ).pack(
            anchor="w",
            padx=15,
            pady=(12, 5)
        )

        input_row = tk.Frame(
            frame,
            bg=self.CARD
        )

        input_row.pack(
            fill="x",
            padx=15,
            pady=(0, 12)
        )

        self.flow_entry = tk.Entry(
            input_row,
            font=("Segoe UI", 15, "bold"),
            width=8,
            justify="center",
            relief="flat",
            borderwidth=0
        )

        self.flow_entry.insert(
            0,
            str(self.pump.target_flow_rate)
        )

        self.flow_entry.pack(
            side="left",
            ipady=6
        )

        tk.Label(
            input_row,
            text="mL/hr",
            font=("Segoe UI", 9),
            fg=self.MUTED,
            bg=self.CARD
        ).pack(
            side="left",
            padx=8
        )

        tk.Button(
            input_row,
            text="SET",
            command=self.set_flow_rate,
            font=("Segoe UI", 8, "bold"),
            fg="white",
            bg=self.BLUE,
            activebackground="#2563EB",
            activeforeground="white",
            relief="flat",
            borderwidth=0,
            padx=10,
            pady=6,
            cursor="hand2"
        ).pack(
            side="right"
        )

    # ==================================================
    # VALUE CARD
    # ==================================================

    def make_value(
        self,
        parent,
        title,
        value,
        unit
    ):

        frame = tk.Frame(
            parent,
            bg=self.CARD
        )

        frame.pack(
            fill="x",
            padx=15,
            pady=5
        )

        tk.Label(
            frame,
            text=title,
            font=("Segoe UI", 8, "bold"),
            fg=self.MUTED,
            bg=self.CARD
        ).pack(
            anchor="w",
            padx=15,
            pady=(12, 2)
        )

        value_label = tk.Label(
            frame,
            text=value,
            font=("Segoe UI", 19, "bold"),
            fg=self.TEXT,
            bg=self.CARD
        )

        value_label.pack(
            anchor="w",
            padx=15
        )

        tk.Label(
            frame,
            text=unit,
            font=("Segoe UI", 8),
            fg=self.MUTED,
            bg=self.CARD
        ).pack(
            anchor="w",
            padx=15,
            pady=(0, 10)
        )

        return value_label

    # ==================================================
    # SETTINGS PAGE
    # ==================================================

    def show_settings(self):

        self.clear_page()

        self.build_header(
            settings_button=False
        )

        body = tk.Frame(
            self.container,
            bg=self.BG
        )

        body.pack(
            fill="both",
            expand=True,
            padx=50,
            pady=40
        )

        tk.Label(
            body,
            text="Attack Simulation",
            font=("Segoe UI", 24, "bold"),
            fg=self.TEXT,
            bg=self.BG
        ).pack()

        tk.Label(
            body,
            text="Inject simulated sensor attacks for testing the detection system.",
            font=("Segoe UI", 11),
            fg=self.MUTED,
            bg=self.BG
        ).pack(
            pady=(5, 35)
        )

        card_row = tk.Frame(
            body,
            bg=self.BG
        )

        card_row.pack(
            fill="x"
        )

        self.attack_card(
            card_row,
            "SPIKE",
            "Inject a sudden abnormal jump in sensor measurements.",
            self.pump.inject_spike
        )

        self.attack_card(
            card_row,
            "DRIFT",
            "Gradually shift measurements away from their expected values.",
            self.pump.inject_drift
        )

        self.attack_card(
            card_row,
            "REPLAY",
            "Replace current readings with previously recorded sensor values.",
            self.pump.inject_replay
        )

        tk.Button(
            body,
            text="CLEAR ACTIVE ATTACK",
            command=self.pump.clear_attack,
            font=("Segoe UI", 10, "bold"),
            fg="white",
            bg=self.RED,
            activebackground="#DC2626",
            activeforeground="white",
            relief="flat",
            borderwidth=0,
            padx=25,
            pady=10,
            cursor="hand2"
        ).pack(
            pady=35
        )

        self.attack_status = tk.Label(
            body,
            text=f"Active Simulation: {self.pump.attack}",
            font=("Segoe UI", 12, "bold"),
            fg=self.YELLOW,
            bg=self.BG
        )

        self.attack_status.pack()

    # ==================================================
    # ATTACK CARD
    # ==================================================

    def attack_card(
        self,
        parent,
        title,
        description,
        command
    ):

        card = tk.Frame(
            parent,
            bg=self.PANEL
        )

        card.pack(
            side="left",
            fill="both",
            expand=True,
            padx=8,
            ipady=20
        )

        tk.Label(
            card,
            text=title,
            font=("Segoe UI", 16, "bold"),
            fg=self.TEXT,
            bg=self.PANEL
        ).pack(
            pady=(20, 8)
        )

        tk.Label(
            card,
            text=description,
            font=("Segoe UI", 9),
            fg=self.MUTED,
            bg=self.PANEL,
            wraplength=220,
            justify="center"
        ).pack(
            padx=20,
            pady=5
        )

        tk.Button(
            card,
            text=f"Inject {title.title()}",
            command=command,
            font=("Segoe UI", 9, "bold"),
            fg="white",
            bg=self.BLUE,
            activebackground="#2563EB",
            activeforeground="white",
            relief="flat",
            borderwidth=0,
            padx=18,
            pady=9,
            cursor="hand2"
        ).pack(
            pady=20
        )

    # ==================================================
    # CONTROLS
    # ==================================================

    def start_pump(self):

        self.set_flow_rate()

        self.pump.start()

    def stop_pump(self):

        self.pump.stop()

    def set_flow_rate(self):

        try:

            flow = float(
                self.flow_entry.get()
            )

            if flow <= 0:
                raise ValueError

            self.pump.set_flow_rate(
                flow
            )

        except ValueError:

            messagebox.showerror(
                "Invalid Flow Rate",
                "Enter a positive flow rate."
            )

    # ==================================================
    # UPDATE
    # ==================================================

    def update_gui(self):

        dt = self.update_interval / 1000

        self.pump.update(dt)

        data = self.pump.get_sensor_data()

        # ------------------------------------------
        # Store graph values
        # ------------------------------------------

        self.sample_number += 1

        self.time_data.append(
            self.sample_number
        )

        self.flow_data.append(
            data["actual_flow_rate"]
        )

        self.pressure_data.append(
            data["pressure"]
        )

        # Only show last 50
        self.time_data = self.time_data[-50:]
        self.flow_data = self.flow_data[-50:]
        self.pressure_data = self.pressure_data[-50:]

        # ------------------------------------------
        # Dashboard might not currently exist
        # ------------------------------------------

        if hasattr(self, "flow_display"):

            try:

                self.flow_display.config(
                    text=f"{data['actual_flow_rate']:.2f}"
                )

                self.pressure_display.config(
                    text=f"{data['pressure']:.2f}"
                )

                self.volume_display.config(
                    text=f"{data['volume_infused']:.3f}"
                )

                self.state_display.config(
                    text=data["state"]
                )

                if data["state"] == "RUNNING":

                    self.state_dot.config(
                        fg=self.GREEN
                    )

                else:

                    self.state_dot.config(
                        fg=self.YELLOW
                    )

                if data["alarm"] == "NONE":

                    self.alarm_display.config(
                        text="NORMAL"
                    )

                    self.alarm_dot.config(
                        fg=self.GREEN
                    )

                else:

                    self.alarm_display.config(
                        text=data["alarm"]
                    )

                    self.alarm_dot.config(
                        fg=self.RED
                    )

                # ----------------------------------
                # GRAPH UPDATE
                # ----------------------------------

                self.flow_line.set_data(
                    self.time_data,
                    self.flow_data
                )

                self.pressure_line.set_data(
                    self.time_data,
                    self.pressure_data
                )

                self.axis.relim()
                self.axis.autoscale_view()

                self.canvas.draw_idle()

            except tk.TclError:
                pass

        # ------------------------------------------
        # Settings page attack status
        # ------------------------------------------

        if hasattr(self, "attack_status"):

            try:

                self.attack_status.config(
                    text=
                    f"Active Simulation: "
                    f"{data['attack']}"
                )

            except tk.TclError:
                pass

        self.root.after(
            self.update_interval,
            self.update_gui
        )


def main():

    root = tk.Tk()

    PumpGUI(root)

    root.mainloop()