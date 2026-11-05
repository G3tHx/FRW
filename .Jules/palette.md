# Palette's Journal

## 2025-05-18 - CLI Window Table Empty State and Range Feedback
**Learning:** Terminal CLI utilities displaying dynamic tabular lists (such as window enumerations or processes) often break user expectations when no items are active by either showing collapsed tables or throwing generic exceptions on selection. Providing an explicit empty-state notification within the bordered box layout and displaying dynamic bounds (e.g., `1..N`) directly within interactive prompts significantly reduces input errors and cognitive overhead for terminal users.
**Action:** Always include empty-state rows matching the bordered terminal theme and embed dynamic valid range hints in numerical input prompts.
