# 32x32 Animation Frame Editor

A small Python/Tkinter editor for the gym-session animation format. It uses only the Python standard library.

## Run

From this folder:

```powershell
python frame_editor.py
```

The editor provides:

- Three frame tabs, with optional second and third frames.
- A clickable 32x32 pixel grid for each frame.
- Frame names and an animation name.
- `Clear frame` and `Invert frame` actions.
- Generated `uint32_t[32]` arrays and an `Animation32` variable.
- `Copy C++` and `Save C++...` actions.

Clicking and dragging with the left mouse button activates pixels. Clicking an active pixel toggles it off.
Use the `Enabled` checkboxes beside frames 2 and 3 to control which frames are generated. Hold the right mouse button and drag to move the whole pixel drawing; pixels outside the grid are discarded when the button is released.

The generated output can be copied into `exercise_catalog.cpp`. The animation uses a 400 ms frame duration by default; change the final value in the generated `Animation32` initializer if needed.
