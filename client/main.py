from dataclasses import dataclass, field
from pathlib import Path
import tkinter as tk
from tkinter import ttk


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
    loop_states: list[bool] = field(default_factory=list)
    button_flags: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    extra_btn_modes: list[int] = field(default_factory=lambda: [0, 0, 0, 0])
    exp_channel: int = 0
    exp_cc_num: int = 11
    midi_msgs: list[MidiMessageConfig] = field(default_factory=list)

    @property
    def name(self) -> str:
        return f"{self.bank}{self.preset}"

    def relay_flags(self) -> int:
        flags = 0
        for index, active in enumerate(self.loop_states):
            if active:
                flags |= 1 << (index + 2)
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
            "        },\n"
            "    }"
        )

class LoopSwitcherUI:
    def __init__(self, root):
        self.root = root
        self.root.title("LoopMIDIx")

        self.banks = ["A", "B", "C", "D"]
        self.presets = ["1", "2", "3", "4"]
        self.num_loops = 14
        self.midi_msg_slots = 10
        self.output_path = Path(__file__).resolve().parents[1] / "code" / "switcher_midi" / "main" / "preset_seed.h"

        self.data = {
            b: {
                p: PresetConfig(
                    bank=b,
                    preset=p,
                    loop_states=[False] * self.num_loops,
                    midi_msgs=[MidiMessageConfig() for _ in range(self.midi_msg_slots)],
                )
                for p in self.presets
            }
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

        ttk.Button(btn_frame, text="Export C Seed", command=self.export_c_seed).pack(side="right", padx=(8, 0))
        ttk.Button(btn_frame, text="Send", command=self.print_config).pack(side="right")

    def load_preset_data(self, event=None):
        b = self.selected_bank.get()
        p = self.selected_preset.get()
        states = self.data[b][p].loop_states

        for i in range(self.num_loops):
            self.loop_vars[i].set(states[i])

    def save_preset_data(self):
        b = self.selected_bank.get()
        p = self.selected_preset.get()
        self.data[b][p].loop_states = [v.get() for v in self.loop_vars]

    def print_config(self):
        b = self.selected_bank.get()
        p = self.selected_preset.get()
        preset = self.data[b][p]

        binary_val = preset.relay_flags()

        print(f"Preset {preset.name}")
        print(f"Binary String: {bin(binary_val)}")
        print(f"Integer Value: {binary_val}")
        print("-" * 20)

    def export_c_seed(self):
        self.save_preset_data()

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
            "static const Preset preset_seed[TOTAL_PRESETS] = {",
            # "    [0 ... TOTAL_PRESETS - 1] = PRESET_DEFAULT_INIT,",
        ]

        for bank_index, bank in enumerate(self.banks):
            for preset_index, preset_name in enumerate(self.presets):
                preset = self.data[bank][preset_name]
                if preset.is_default():
                    continue

                array_index = (bank_index * len(self.presets)) + preset_index
                lines.append(f"    [{array_index}] = {preset.to_c_initializer()}, /* {preset.name} */")

        lines.extend([
            "};",
            "",
            "#undef PRESET_DEFAULT_MIDI_MSG",
            "#undef PRESET_DEFAULT_INIT",
        ])

        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        self.output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"Exported preset seed to {self.output_path}")

if __name__ == "__main__":
    root = tk.Tk()
    style = ttk.Style()
    if "clam" in style.theme_names():
        style.theme_use("clam")

    app = LoopSwitcherUI(root)
    root.mainloop()
