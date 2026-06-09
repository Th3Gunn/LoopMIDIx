import json
from dataclasses import dataclass, field, asdict
from pathlib import Path
import tkinter as tk
from tkinter import ttk

BTN_MODES = ["NONE", "SUB_PATCH", "TAP_TEMPO", "MOMENTARY", "STOMP_TOGGLE"]
MIDI_TYPES = ["NONE", "PC", "CC"]

@dataclass
class MidiMessageConfig:
    type: int = 0
    channel: int = 0
    data1: int = 0
    data2: int = 0
    is_tx: int = 1
    uart_num: int = 1

    def is_default(self) -> bool:
        return (
            self.type == 0
            and self.channel == 0
            and self.data1 == 0
            and self.data2 == 0
            and self.is_tx == 1
            and self.uart_num == 1
        )

    def to_c_initializer(self) -> str:
        type_name = {
            0: "MIDI_MSG_NONE",
            1: "MIDI_MSG_PC",
            2: "MIDI_MSG_CC",
        }.get(self.type, str(self.type))

        return (
            "{ .type = %s, .channel = %d, .data1 = %d, .data2 = %d, .is_tx = %d, .uart_num = %d }"
            % (type_name, self.channel, self.data1, self.data2, self.is_tx, self.uart_num)
        )

@dataclass
class PresetConfig:
    bank: str
    preset: str
    loop_states: list[bool] = field(default_factory=lambda: [False] * 12)
    amp_tip: bool = False
    amp_ring: bool = False
    button_flags: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    extra_btn_modes: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    exp_channel: int = 0
    exp_cc_num: int = 11
    midi_msgs: list[MidiMessageConfig] = field(default_factory=lambda: [MidiMessageConfig() for _ in range(10)])

    @property
    def name(self) -> str:
        return f"{self.bank}{self.preset}"

    def relay_flags(self) -> int:
        # LLLLLLLLLLLL00TR format
        flags = 0
        if self.amp_ring:
            flags |= (1 << 0)
        if self.amp_tip:
            flags |= (1 << 1)
            
        for index, active in enumerate(self.loop_states):
            if active:
                flags |= (1 << (index + 4))
        return flags

    def is_default(self) -> bool:
        return (
            self.relay_flags() == 0
            and self.button_flags == [0, 0, 0, 0]
            and self.extra_btn_modes == [0, 0, 0, 0]
            and self.exp_channel == 0
            and self.exp_cc_num == 11
            and all(msg.is_default() for msg in self.midi_msgs)
        )

    def to_c_initializer(self) -> str:
        midi_lines = ",\n".join(
            f"            [{index}] = {msg.to_c_initializer()}"
            for index, msg in enumerate(self.midi_msgs)
        )

        return (
            "{\n"
            f"        .relay_flags = 0x{self.relay_flags():04X},\n"
            f"        .button_flags = {{{', '.join(str(value) for value in self.button_flags)}}},\n"
            f"        .extra_btn_modes = {{{', '.join(str(value) for value in self.extra_btn_modes)}}},\n"
            f"        .exp_channel = {self.exp_channel},\n"
            f"        .exp_cc_num = {self.exp_cc_num},\n"
            "        .midi_msgs = {\n"
            f"{midi_lines}\n"
            "        }\n"
            "    }"
        )

class LoopSwitcherUI:
    def __init__(self, root):
        self.root = root
        self.root.title("LoopMIDIx - Preset Configuration")
        self.root.geometry("680x520")

        self.banks = [str(i) for i in range(1, 129)]
        self.presets = ["A", "B", "C", "D"]
        self.num_loops = 12
        self.midi_msg_slots = 10  

        base_dir = Path(__file__).resolve().parent
        self.config_path = base_dir / "loopmidix_config.json"
        
        self.output_path = base_dir.parent / "code" / "switcher_midi" / "main" / "preset_seed.h"

        self.data = {
            b: {
                p: PresetConfig(bank=b, preset=p) for p in self.presets
            }
            for b in self.banks
        }

        self.load_from_json()

        self.selected_bank = tk.StringVar(value=self.banks[0])
        self.selected_preset = tk.StringVar(value=self.presets[0])

        self.loop_vars = [tk.BooleanVar() for _ in range(self.num_loops)]
        self.amp_tip_var = tk.BooleanVar()
        self.amp_ring_var = tk.BooleanVar()

        self.extra_btn_vars = [tk.StringVar(value=BTN_MODES[0]) for _ in range(4)]
        self.exp_chan_var = tk.IntVar(value=0)
        self.exp_cc_var = tk.IntVar(value=11)

        self.midi_vars = []
        for _ in range(self.midi_msg_slots):
            self.midi_vars.append({
                "type": tk.StringVar(value="NONE"),
                "is_tx": tk.StringVar(value="TX"),
                "uart": tk.StringVar(value="1"),
                "chan": tk.IntVar(value=0),
                "d1": tk.IntVar(value=0),
                "d2": tk.IntVar(value=0)
            })

        self.loading = False
        self.current_loaded_bank = self.banks[0]
        self.current_loaded_preset = self.presets[0]

        self.setup_ui()
        self.attach_traces()
        self.load_preset_data()

    def load_from_json(self):
        """Loads configuration from a JSON file into self.data if it exists."""
        if not self.config_path.exists():
            print("No existing configuration found. Starting with defaults.")
            return

        try:
            with open(self.config_path, "r", encoding="utf-8") as f:
                raw_data = json.load(f)

            for b, presets in raw_data.items():
                if b not in self.data:
                    continue
                for p, p_data in presets.items():
                    if p not in self.data[b]:
                        continue
                    

                    midi_dicts = p_data.pop("midi_msgs", [])
                    midi_msgs = [MidiMessageConfig(**m) for m in midi_dicts]
                    

                    self.data[b][p] = PresetConfig(**p_data, midi_msgs=midi_msgs)
            
            print(f"Successfully loaded configuration from {self.config_path}")
        except Exception as e:
            print(f"Failed to load configuration JSON: {e}")

    def save_to_json(self):
        """Serializes the current state into a JSON file."""
        export_data = {}
        for b, presets in self.data.items():
            export_data[b] = {}
            for p, preset in presets.items():
                export_data[b][p] = asdict(preset)
        
        try:
            with open(self.config_path, "w", encoding="utf-8") as f:
                json.dump(export_data, f, indent=4)
            print(f"Saved application state to {self.config_path}")
        except Exception as e:
            print(f"Failed to save configuration JSON: {e}")

    def setup_ui(self):

        top_frame = ttk.LabelFrame(self.root, text=" Target Memory ", padding=10)
        top_frame.pack(fill="x", padx=10, pady=5)

        ttk.Label(top_frame, text="Bank:").pack(side="left")
        bank_combo = ttk.Combobox(top_frame, textvariable=self.selected_bank, values=self.banks, width=5, state="readonly")
        bank_combo.pack(side="left", padx=5)
        bank_combo.bind("<<ComboboxSelected>>", self.load_preset_data)

        ttk.Label(top_frame, text="Preset:").pack(side="left", padx=(15, 0))
        preset_combo = ttk.Combobox(top_frame, textvariable=self.selected_preset, values=self.presets, width=5, state="readonly")
        preset_combo.pack(side="left", padx=5)
        preset_combo.bind("<<ComboboxSelected>>", self.load_preset_data)


        notebook = ttk.Notebook(self.root)
        notebook.pack(fill="both", expand=True, padx=10, pady=5)


        tab_relays = ttk.Frame(notebook, padding=10)
        notebook.add(tab_relays, text="Loops & Amps")

        loop_lf = ttk.LabelFrame(tab_relays, text=" Guitar FX Loops ", padding=10)
        loop_lf.pack(fill="x", pady=(0, 10))
        for i in range(self.num_loops):
            ttk.Checkbutton(loop_lf, text=f"Loop {i+1}", variable=self.loop_vars[i]).grid(row=i//6, column=i%6, padx=15, pady=10, sticky="w")

        amp_lf = ttk.LabelFrame(tab_relays, text=" Amplifier Switching (Relays) ", padding=10)
        amp_lf.pack(fill="x")
        ttk.Checkbutton(amp_lf, text="FS Tip Active", variable=self.amp_tip_var).pack(side="left", padx=15, pady=5)
        ttk.Checkbutton(amp_lf, text="FS Ring Active", variable=self.amp_ring_var).pack(side="left", padx=15, pady=5)


        tab_hw = ttk.Frame(notebook, padding=10)
        notebook.add(tab_hw, text="Switches & Expression")

        btn_lf = ttk.LabelFrame(tab_hw, text=" Extra Assignable Buttons ", padding=10)
        btn_lf.pack(fill="x", pady=(0, 10))
        for i in range(4):
            ttk.Label(btn_lf, text=f"Button {i+1} Mode:").grid(row=i, column=0, padx=10, pady=5, sticky="e")
            ttk.Combobox(btn_lf, textvariable=self.extra_btn_vars[i], values=BTN_MODES, state="readonly", width=15).grid(row=i, column=1, padx=5, pady=5, sticky="w")

        exp_lf = ttk.LabelFrame(tab_hw, text=" Expression Pedal Binding ", padding=10)
        exp_lf.pack(fill="x")
        ttk.Label(exp_lf, text="MIDI Channel:").grid(row=0, column=0, padx=10, pady=5, sticky="e")
        ttk.Spinbox(exp_lf, from_=0, to=15, textvariable=self.exp_chan_var, width=5).grid(row=0, column=1, padx=5, pady=5, sticky="w")
        ttk.Label(exp_lf, text="Target CC Number:").grid(row=1, column=0, padx=10, pady=5, sticky="e")
        ttk.Spinbox(exp_lf, from_=0, to=127, textvariable=self.exp_cc_var, width=5).grid(row=1, column=1, padx=5, pady=5, sticky="w")


        tab_midi = ttk.Frame(notebook, padding=10)
        notebook.add(tab_midi, text="MIDI Messaging")

        headers = ["Slot", "Type", "Direction", "Output UART", "Channel", "CC/PC Data 1", "Data 2"]
        for col, h in enumerate(headers):
            ttk.Label(tab_midi, text=h, font=("", 9, "bold")).grid(row=0, column=col, padx=8, pady=5)

        for i in range(self.midi_msg_slots):
            ttk.Label(tab_midi, text=f"Msg {i+1}:").grid(row=i+1, column=0, padx=8, pady=3)
            ttk.Combobox(tab_midi, textvariable=self.midi_vars[i]["type"], values=MIDI_TYPES, state="readonly", width=6).grid(row=i+1, column=1)
            ttk.Combobox(tab_midi, textvariable=self.midi_vars[i]["is_tx"], values=["TX", "RX"], state="readonly", width=4).grid(row=i+1, column=2)
            ttk.Combobox(tab_midi, textvariable=self.midi_vars[i]["uart"], values=["1", "2", "3"], state="readonly", width=4).grid(row=i+1, column=3)
            ttk.Spinbox(tab_midi, from_=0, to=15, textvariable=self.midi_vars[i]["chan"], width=5).grid(row=i+1, column=4)
            ttk.Spinbox(tab_midi, from_=0, to=127, textvariable=self.midi_vars[i]["d1"], width=5).grid(row=i+1, column=5)
            ttk.Spinbox(tab_midi, from_=0, to=127, textvariable=self.midi_vars[i]["d2"], width=5).grid(row=i+1, column=6)


        btn_frame = ttk.Frame(self.root, padding=10)
        btn_frame.pack(fill="x")
        ttk.Button(btn_frame, text="Save & Generate C Seed", command=self.export_c_seed).pack(side="right", padx=(8, 0))
        ttk.Button(btn_frame, text="Send Verification Config", command=self.print_config).pack(side="right")

    def attach_traces(self):
        def trace_cb(*args):
            self.save_current_preset()

        for var in self.loop_vars: var.trace_add("write", trace_cb)
        self.amp_tip_var.trace_add("write", trace_cb)
        self.amp_ring_var.trace_add("write", trace_cb)

        for var in self.extra_btn_vars: var.trace_add("write", trace_cb)
        self.exp_chan_var.trace_add("write", trace_cb)
        self.exp_cc_var.trace_add("write", trace_cb)

        for mv in self.midi_vars:
            for key in mv:
                mv[key].trace_add("write", trace_cb)

    def load_preset_data(self, event=None):
        self.loading = True
        
        b = self.selected_bank.get()
        p = self.selected_preset.get()
        
        self.current_loaded_bank = b
        self.current_loaded_preset = p
        
        preset = self.data[b][p]


        for i in range(self.num_loops):
            self.loop_vars[i].set(preset.loop_states[i])

        self.amp_tip_var.set(preset.amp_tip)
        self.amp_ring_var.set(preset.amp_ring)

        for i in range(4):
            self.extra_btn_vars[i].set(BTN_MODES[preset.extra_btn_modes[i]])

        self.exp_chan_var.set(preset.exp_channel)
        self.exp_cc_var.set(preset.exp_cc_num)

        for i, mv in enumerate(self.midi_vars):
            msg = preset.midi_msgs[i]
            mv["type"].set(MIDI_TYPES[msg.type])
            mv["is_tx"].set("TX" if msg.is_tx else "RX")
            mv["uart"].set(str(msg.uart_num))
            mv["chan"].set(msg.channel)
            mv["d1"].set(msg.data1)
            mv["d2"].set(msg.data2)

        self.loading = False

    def save_current_preset(self):
        if self.loading:
            return

        b = self.current_loaded_bank
        p = self.current_loaded_preset
        preset = self.data[b][p]

        preset.loop_states = [v.get() for v in self.loop_vars]
        preset.amp_tip = self.amp_tip_var.get()
        preset.amp_ring = self.amp_ring_var.get()

        for i, v in enumerate(self.extra_btn_vars):
            try: preset.extra_btn_modes[i] = BTN_MODES.index(v.get())
            except ValueError: preset.extra_btn_modes[i] = 0

        try: preset.exp_channel = self.exp_chan_var.get()
        except tk.TclError: pass

        try: preset.exp_cc_num = self.exp_cc_var.get()
        except tk.TclError: pass

        for i, mv in enumerate(self.midi_vars):
            try: preset.midi_msgs[i].type = MIDI_TYPES.index(mv["type"].get())
            except ValueError: preset.midi_msgs[i].type = 0

            preset.midi_msgs[i].is_tx = 1 if mv["is_tx"].get() == "TX" else 0

            try: preset.midi_msgs[i].uart_num = int(mv["uart"].get())
            except ValueError: preset.midi_msgs[i].uart_num = 1

            try: preset.midi_msgs[i].channel = mv["chan"].get()
            except tk.TclError: pass

            try: preset.midi_msgs[i].data1 = mv["d1"].get()
            except tk.TclError: pass

            try: preset.midi_msgs[i].data2 = mv["d2"].get()
            except tk.TclError: pass

    def print_config(self):
        b = self.current_loaded_bank
        p = self.current_loaded_preset
        preset = self.data[b][p]

        binary_val = preset.relay_flags()
        print(f"\nTargeting Setup {preset.name}")
        print(f"Computed Integer Relay Mask: {binary_val}")
        print(f"Shift Register Bin Packet (16 bit): {binary_val:016b}")
        print("-" * 35)

    def export_c_seed(self):

        self.save_current_preset()
        

        self.save_to_json()


        lines = [
            "/* Generated by client/main.py. Do not edit by hand. */",
            "#pragma once",
            "",
            "#define PRESET_DEFAULT_MIDI_MSG { .type = MIDI_MSG_NONE, .channel = 0, .data1 = 0, .data2 = 0, .is_tx = 1, .uart_num = 1 }",
            "#define PRESET_DEFAULT_INIT { \\",
            "    .relay_flags = 0, \\",
            "    .button_flags = {0, 0, 0, 0}, \\",
            "    .extra_btn_modes = {0, 0, 0, 0}, \\",
            "    .exp_channel = 0, \\",
            "    .exp_cc_num = 11, \\",
            "    .midi_msgs = { [0 ... 9] = PRESET_DEFAULT_MIDI_MSG } \\",
            "}",
            "",
            "static const Preset preset_seed[TOTAL_PRESETS] = {"
        ]


        for bank_index, bank in enumerate(self.banks):
            for preset_index, preset_name in enumerate(self.presets):
                preset = self.data[bank][preset_name]
                array_index = (bank_index * len(self.presets)) + preset_index

                if preset.is_default():
                    lines.append(f"    [{array_index}] = PRESET_DEFAULT_INIT,")
                else:
                    lines.append(f"    [{array_index}] = {preset.to_c_initializer()}, /* {preset.name} */")

        lines.extend([
            "};",
            "",
            "#undef PRESET_DEFAULT_MIDI_MSG",
            "#undef PRESET_DEFAULT_INIT",
        ])

        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        self.output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"Exported successfully structured C preset seed to: {self.output_path}")

if __name__ == "__main__":
    root = tk.Tk()
    style = ttk.Style()
    if "clam" in style.theme_names():
        style.theme_use("clam")

    app = LoopSwitcherUI(root)
    root.mainloop()