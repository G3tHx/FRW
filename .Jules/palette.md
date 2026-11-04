# Palette's Journal - Critical UX Learnings

## 2026-10-26 - Present Baseline State Before User Input Decisions
**Learning:** In terminal-based interactive configuration workflows, prompting users for target adjustments (e.g., setting a new width and height) without displaying the current state leaves users guessing about baseline geometry and aspect ratios. Providing the active context directly inside the input prompt box drastically reduces user uncertainty and input error.
**Action:** Always inspect and display existing element metrics or properties before prompting the user to modify them in console and CLI utilities.
