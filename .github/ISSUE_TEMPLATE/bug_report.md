---
name: Bug report
about: Create a report to help us improve
title: "[Bug] Epson <Model>: <Short description>."
labels: bug, needs-triage
assignees: RxNaison

---

**Describe the bug**
A clear and concise description of what the bug is. (e.g., "The program exits with ZERO_ACK_FAILURE after sending all packets," or "EWR cannot detect my printer on USB.")

**Printer Information (CRITICAL):**
 * Printer Model: [e.g., Epson ET-2826]
 * Printer type: [Single-function / Multi-function (print+scan+copy)]
 * Did the printer show any lights or LCD error messages before/after the reset attempt?
 * Is this a fresh driver install or a long-standing driver setup?
 * Was the official Epson driver package installed at the time of the attempt?

**To Reproduce**
Steps to reproduce the behavior:
 * Connected printer via USB to [Native PC / VirtualBox / VMware]
 * USB port type: [Motherboard rear USB / Front panel USB / USB Hub / USB-C adapter]
 * Ran the executable in the terminal using command: ...
 * Selected printer payload: ...
 * See error

**Expected behavior**
A clear and concise description of what you expected to happen.

**Console Output**
Please copy and paste the **exact, full** output from your terminal/command prompt, from the very first line to the last.
```
[Paste full terminal output here]
```

**Diagnostic Trace Log (`ewr_trace.log`)**
After every run, EWR generates a file called `ewr_trace.log` in the same directory as the executable. **This file is essential for remote debugging.** Please attach it to this issue or paste its contents below.
```
[Paste the entire contents of ewr_trace.log here]
```
> **What does `ewr_trace.log` contain?**
> It records every step of the USB communication pipeline: all enumerated device paths, interface selection logic (which USB class was scanned, which interface index was chosen and why), every packet sent/received with hex data and ACK status, and human-readable error strings for any Windows/Linux failures.

**Desktop / Environment:**
 * OS: [e.g., Windows 11 24H2, Ubuntu 24.04, Arch Linux]
 * EWR Version: [e.g., v1.2.1]
 * Connection: [e.g., Direct USB to motherboard, USB Hub, USB Passthrough to VM]
 * Did you run with correct privileges? (`sudo` on Linux)

**Pre-flight Checklist (Please check all before submitting):**
- [ ] I am using the **latest release** of EWR.
- [ ] I have read the `README.md` and understand the hardware risks.
- [ ] I ran the program with elevated privileges (`sudo` on Linux).
- [ ] My printer is **physically powered on** and **connected via USB**, not just plugged into power.
- [ ] I have attached or pasted the contents of `ewr_trace.log`.
- [ ] I have searched the existing issues to make sure this hasn't been reported already.

**Common Fixes to Try Before Submitting:**
> Before opening an issue, please try these steps — they resolve most reported problems:
> 1. **Reinstall official Epson drivers** from [epson.com/support](https://epson.com/support) (this fixes >80% of "zero ACK" failures on multi-function printers).
> 2. **Connect to a rear motherboard USB port** — avoid USB hubs, front-panel ports, and USB-C adapters.
> 3. **Power-cycle the printer** (unplug from wall power for 30 seconds, then plug back in).
> 4. **Close all other Epson software** (Epson Scan, Epson Event Manager, etc.) before running EWR.

**Additional context**
Add any other context about the problem here (e.g., "I heard a grinding noise," "The printer was showing a waste ink pad error on its LCD," or "I had to manually kill the terminal").
