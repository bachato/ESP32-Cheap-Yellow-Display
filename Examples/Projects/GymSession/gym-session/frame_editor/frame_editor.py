import re
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

GRID_SIZE = 32
CELL_SIZE = 16
MAX_FRAMES = 3


class FrameEditor(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("32x32 Animation Frame Editor")
        self.resizable(False, False)

        self.animation_name = tk.StringVar(value="myAnimation")
        self.frame_names = [tk.StringVar(value=f"frame{i + 1}") for i in range(MAX_FRAMES)]
        self.frame_enabled = [tk.BooleanVar(value=True) for _ in range(MAX_FRAMES)]
        self.frames = [[False] * (GRID_SIZE * GRID_SIZE) for _ in range(MAX_FRAMES)]
        self.active_frame = 0
        self.cells = []
        self.shift_start = None
        self.shift_source = None

        self._build_ui()
        self._select_frame(0)
        self._refresh_output()

    def _build_ui(self):
        main = ttk.Frame(self, padding=10)
        main.grid(row=0, column=0)

        settings = ttk.Frame(main)
        settings.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 8))
        ttk.Label(settings, text="Animation name:").grid(row=0, column=0, padx=(0, 5))
        name_entry = ttk.Entry(settings, textvariable=self.animation_name, width=24)
        name_entry.grid(row=0, column=1, sticky="w")
        name_entry.bind("<KeyRelease>", lambda _event: self._refresh_output())
        ttk.Label(settings, text="Frame names are used as C++ variable names.").grid(
            row=0, column=2, padx=(12, 0), sticky="w"
        )

        self.notebook = ttk.Notebook(main)
        self.notebook.grid(row=1, column=0, sticky="n")
        self.frame_tabs = []
        for index in range(MAX_FRAMES):
            tab = ttk.Frame(self.notebook, padding=6)
            self.notebook.add(tab, text=f"Frame {index + 1}")
            self.frame_tabs.append(tab)
            self._build_frame_tab(tab, index)
        self.notebook.bind("<<NotebookTabChanged>>", self._tab_changed)

        controls = ttk.Frame(main)
        controls.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        ttk.Button(controls, text="Copy previous", command=self._copy_previous_frame).grid(row=0, column=0, padx=(0, 4))
        ttk.Button(controls, text="Clear frame", command=self._clear_frame).grid(row=0, column=1, padx=4)
        ttk.Button(controls, text="Invert frame", command=self._invert_frame).grid(row=0, column=2, padx=4)
        ttk.Button(controls, text="Copy C++", command=self._copy_output).grid(row=0, column=3, padx=4)
        ttk.Button(controls, text="Save C++...", command=self._save_output).grid(row=0, column=4, padx=(4, 0))

        output_frame = ttk.LabelFrame(main, text="Generated C++", padding=6)
        output_frame.grid(row=3, column=0, pady=(8, 0), sticky="ew")
        self.output = tk.Text(output_frame, width=72, height=18, font=("Consolas", 9), wrap="none")
        self.output.grid(row=0, column=0)
        scrollbar = ttk.Scrollbar(output_frame, orient="vertical", command=self.output.yview)
        scrollbar.grid(row=0, column=1, sticky="ns")
        self.output.configure(yscrollcommand=scrollbar.set)

    def _build_frame_tab(self, tab, index):
        name_row = ttk.Frame(tab)
        name_row.grid(row=0, column=0, sticky="w", pady=(0, 6))
        ttk.Label(name_row, text="Frame name:").grid(row=0, column=0, padx=(0, 5))
        entry = ttk.Entry(name_row, textvariable=self.frame_names[index], width=22)
        entry.grid(row=0, column=1)
        entry.bind("<KeyRelease>", lambda _event: self._refresh_output())
        if index > 0:
            ttk.Checkbutton(
                name_row,
                text="Enabled",
                variable=self.frame_enabled[index],
                command=self._refresh_output,
            ).grid(row=0, column=2, padx=(8, 0))

        canvas = tk.Canvas(
            tab,
            width=GRID_SIZE * CELL_SIZE,
            height=GRID_SIZE * CELL_SIZE,
            background="white",
            highlightthickness=1,
            highlightbackground="#777777",
        )
        canvas.grid(row=1, column=0)
        canvas.bind("<Button-1>", lambda event, frame=index: self._start_paint(event, frame))
        canvas.bind("<B1-Motion>", lambda event, frame=index: self._paint_cell(event, frame))
        canvas.bind("<Button-3>", lambda event, frame=index: self._start_shift(event, frame))
        canvas.bind("<B3-Motion>", lambda event, frame=index: self._preview_shift(event, frame))
        canvas.bind(
            "<ButtonRelease-3>", lambda event, frame=index: self._finish_shift(event, frame)
        )
        tab.canvas = canvas

    def _tab_changed(self, _event):
        self._select_frame(self.notebook.index(self.notebook.select()))

    def _select_frame(self, index):
        self.active_frame = index
        self._draw_grid()

    def _cell_index(self, event):
        column = event.x // CELL_SIZE
        row = event.y // CELL_SIZE
        if 0 <= column < GRID_SIZE and 0 <= row < GRID_SIZE:
            return row * GRID_SIZE + column
        return None

    def _start_paint(self, event, frame):
        index = self._cell_index(event)
        if index is not None:
            self.paint_value = not self.frames[frame][index]
            self._paint_cell(event, frame)

    def _paint_cell(self, event, frame):
        index = self._cell_index(event)
        if index is not None and self.frames[frame][index] != self.paint_value:
            self.frames[frame][index] = self.paint_value
            self._draw_grid()
            self._refresh_output()

    def _start_shift(self, event, frame):
        self.active_frame = frame
        self.shift_start = (event.x // CELL_SIZE, event.y // CELL_SIZE)
        self.shift_source = self.frames[frame].copy()

    def _shift_offset(self, event):
        if self.shift_start is None:
            return 0, 0
        return event.x // CELL_SIZE - self.shift_start[0], event.y // CELL_SIZE - self.shift_start[1]

    def _shifted_frame(self, source, offset):
        shifted = [False] * (GRID_SIZE * GRID_SIZE)
        offset_x, offset_y = offset
        for row in range(GRID_SIZE):
            for column in range(GRID_SIZE):
                if not source[row * GRID_SIZE + column]:
                    continue
                shifted_row = row + offset_y
                shifted_column = column + offset_x
                if 0 <= shifted_row < GRID_SIZE and 0 <= shifted_column < GRID_SIZE:
                    shifted[shifted_row * GRID_SIZE + shifted_column] = True
        return shifted

    def _preview_shift(self, event, frame):
        if self.shift_source is None:
            return
        self._draw_grid(self._shifted_frame(self.shift_source, self._shift_offset(event)))

    def _finish_shift(self, event, frame):
        if self.shift_source is None:
            return
        self.frames[frame] = self._shifted_frame(self.shift_source, self._shift_offset(event))
        self.shift_start = None
        self.shift_source = None
        self._draw_grid()
        self._refresh_output()

    def _draw_grid(self, frame=None):
        canvas = self.frame_tabs[self.active_frame].canvas
        canvas.delete("all")
        frame = self.frames[self.active_frame] if frame is None else frame
        for row in range(GRID_SIZE):
            for column in range(GRID_SIZE):
                left = column * CELL_SIZE
                top = row * CELL_SIZE
                color = "#111111" if frame[row * GRID_SIZE + column] else "white"
                canvas.create_rectangle(
                    left,
                    top,
                    left + CELL_SIZE,
                    top + CELL_SIZE,
                    fill=color,
                    outline="#cccccc",
                )

    def _clear_frame(self):
        self.frames[self.active_frame] = [False] * (GRID_SIZE * GRID_SIZE)
        self._draw_grid()
        self._refresh_output()

    def _invert_frame(self):
        self.frames[self.active_frame] = [not value for value in self.frames[self.active_frame]]
        self._draw_grid()
        self._refresh_output()

    def _copy_previous_frame(self):
        if self.active_frame == 0:
            return
        self.frames[self.active_frame] = self.frames[self.active_frame - 1].copy()
        self._draw_grid()
        self._refresh_output()

    def _identifier(self, value, fallback):
        value = re.sub(r"[^a-zA-Z0-9_]", "_", value.strip())
        if not value:
            value = fallback
        if value[0].isdigit():
            value = "_" + value
        return value

    def _frame_values(self, frame):
        if len(frame) != GRID_SIZE * GRID_SIZE:
            raise ValueError(f"A frame must contain exactly {GRID_SIZE}x{GRID_SIZE} pixels")
        values = []
        for row in range(GRID_SIZE):
            value = 0
            for column in range(GRID_SIZE):
                if not frame[row * GRID_SIZE + column]:
                    value |= 0x80000000 >> column
            values.append(value)
        return values

    def _generated_cpp(self):
        animation = self._identifier(self.animation_name.get(), "myAnimation")
        enabled_frames = [
            (self._identifier(name.get(), f"frame{i + 1}"), frame)
            for i, (name, frame, enabled) in enumerate(
                zip(self.frame_names, self.frames, self.frame_enabled)
            )
            if enabled.get()
        ]
        lines = ["// Generated by frame_editor.py", "#include \"exercise_catalog.h\"", ""]
        for frame_name, frame in enabled_frames:
            lines.append(f"const uint32_t {frame_name}[32] = {{")
            values = self._frame_values(frame)
            for row in range(0, GRID_SIZE, 4):
                group = ", ".join(f"0x{values[row + offset]:08X}" for offset in range(4))
                suffix = "," if row + 4 < GRID_SIZE else ""
                lines.append(f"  {group}{suffix}")
            lines.append("};")
            lines.append("")
        lines.append(f"const Animation32 {animation} = {{")
        frame_entries = ", ".join(
            f"{{{name}, sizeof({name}) / sizeof({name}[0])}}" for name, _frame in enabled_frames
        )
        lines.append("  {" + frame_entries + "},")
        lines.append(f"  {len(enabled_frames)}, 400")
        lines.append("};")
        return "\n".join(lines) + "\n"

    def _refresh_output(self):
        if not hasattr(self, "output"):
            return
        self.output.delete("1.0", tk.END)
        self.output.insert("1.0", self._generated_cpp())

    def _copy_output(self):
        self.clipboard_clear()
        self.clipboard_append(self._generated_cpp())
        self.update()

    def _save_output(self):
        path = filedialog.asksaveasfilename(
            title="Save generated C++",
            defaultextension=".cpp",
            filetypes=[("C++ source", "*.cpp"), ("Text file", "*.txt")],
        )
        if not path:
            return
        Path(path).write_text(self._generated_cpp(), encoding="utf-8")
        messagebox.showinfo("Saved", f"Generated C++ saved to:\n{path}")


if __name__ == "__main__":
    FrameEditor().mainloop()
