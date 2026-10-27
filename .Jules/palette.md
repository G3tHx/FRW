## 2025-05-10 - CLI Input Recovery and Guidance
**Learning:** In terminal user interfaces, unvalidated `std::cin` or `std::wcin` calls for numerical options cause stream fail states upon non-numeric input, leading to unexpected input loops or program termination.
**Action:** Always wrap interactive CLI integer inputs in a stream recovery helper function (`ReadValidInt`) that resets `std::wcin.clear()` and clears the buffer with `std::wcin.ignore()` while providing immediate feedback in the application's visual theme style.
