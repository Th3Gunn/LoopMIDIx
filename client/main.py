import tkinter as tk
from tkinter import ttk

class LoopSwitcherUI:
    def __init__(self, root):
        self.root = root
        self.root.title("LoopMIDIx")

        self.banks = ["A", "B", "C", "D"]
        self.presets = ["1", "2", "3", "4"]
        self.num_loops = 14

        self.data = {
            b: {p: [False] * self.num_loops for p in self.presets}
            for b in self.banks
        }

        self.selected_bank = tk.StringVar(value=self.banks[0])
        self.selected_preset = tk.StringVar(value=self.presets[0])
        self.loop_vars = [tk.BooleanVar() for _ in range(self.num_loops)]

        self.setup_ui()

    def setup_ui(self):
        top_frame = ttk.LabelFrame(self.root, text=" Select Bank & Preset ", padding=10)
        top_frame.pack(fill="x", padx=10, pady=5)

        ttk.Label(top_frame, text="Bank:").pack(side="left")
        bank_combo = ttk.Combobox(top_frame, textvariable=self.selected_bank, values=self.banks, width=5)
        bank_combo.pack(side="left", padx=5)
        bank_combo.bind("<<ComboboxSelected>>", self.load_preset_data)

        ttk.Label(top_frame, text="Preset:").pack(side="left", padx=(15, 0))
        preset_combo = ttk.Combobox(top_frame, textvariable=self.selected_preset, values=self.presets, width=5)
        preset_combo.pack(side="left", padx=5)
        preset_combo.bind("<<ComboboxSelected>>", self.load_preset_data)

        loop_frame = ttk.LabelFrame(self.root, text=" Active Loops ", padding=10)
        loop_frame.pack(fill="both", expand=True, padx=10, pady=5)

        for i in range(self.num_loops):
            row = i // 7
            col = i % 7
            cb = ttk.Checkbutton(
                loop_frame,
                text=f"Loop {i+1}",
                variable=self.loop_vars[i],
                command=self.save_preset_data
            )
            cb.grid(row=row, column=col, padx=10, pady=10, sticky="w")

        btn_frame = ttk.Frame(self.root, padding=10)
        btn_frame.pack(fill="x")

        ttk.Button(btn_frame, text="Send", command=self.print_config).pack(side="right")

    def load_preset_data(self, event=None):
        b = self.selected_bank.get()
        p = self.selected_preset.get()
        states = self.data[b][p]

        for i in range(self.num_loops):
            self.loop_vars[i].set(states[i])

    def save_preset_data(self):
        b = self.selected_bank.get()
        p = self.selected_preset.get()
        self.data[b][p] = [v.get() for v in self.loop_vars]

    def print_config(self):
        b = self.selected_bank.get()
        p = self.selected_preset.get()
        states = self.data[b][p]

        binary_val = sum(1 << i for i, val in enumerate(states) if val)

        print(f"Bank {b} - Preset {p}")
        print(f"Binary String: {bin(binary_val)}")
        print(f"Integer Value: {binary_val}")
        print("-" * 20)

if __name__ == "__main__":
    root = tk.Tk()
    style = ttk.Style()
    if "clam" in style.theme_names():
        style.theme_use("clam")

    app = LoopSwitcherUI(root)
    root.mainloop()
