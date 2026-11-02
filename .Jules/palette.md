# Palette's Journal

## 2026-11-02 - Console UX Status and Empty State in Terminal App
**Learning:** In terminal CLI applications using box-drawing borders and colored ANSI themes (like the Metro 2033 theme here), missing interactive feedback on selection creates uncertainty. Providing symmetric confirmation feedback across all selection paths (`by index`, `by title`, `by process`) ensures consistent mental models.
**Action:** When working with console user interfaces, make sure all input selection branches provide matching acknowledgement feedback (`[ ✔ ]`) and that empty lists display informative empty-state messages rather than empty border boxes.
