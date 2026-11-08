# Palette's Journal

## 2026-11-06 - Contextual Baseline Disclosure in Terminal Geometry Prompts
**Learning:** In interactive CLI and terminal utilities that prompt users for geometric dimension overrides (such as window width and height), asking for new dimensions without displaying existing dimensions leaves users guessing and risks accidental distortion of aspect ratios. Providing immediate target selection feedback and disclosing current baseline dimensions directly within the prompt boundary creates an intuitive, accessible flow.
**Action:** Always present active entity baseline metrics directly within input prompt frames before requesting parameter changes.
