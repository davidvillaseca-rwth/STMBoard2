# NUCLEO-G474RE + mediT "STM32 Seminar V3" shield: detailed notes

Everything found and built so far. "Confirmed" means tested on the real board.

## 1. Hardware overview

| Item | Details |
|---|---|
| MCU board | NUCLEO-G474RE (STM32G474RE, Cortex-M4F, 170 MHz max) |
| Shield | Blue mediT "STM32 Seminar V3" Arduino shield (no schematic available, pin map found by testing) |
| Shield parts | 4 push buttons (D-pad), potentiometer, piezo buzzer, 2 LEDs (LED2 green, LED3 yellow), infrared pulse sensor with IR LED (LED1), an amplifier-type IC next to the sensor |
| IDE | STM32CubeIDE 2.3.0 (CubeMX is NOT built in since 2.0; use standalone STM32CubeMX 6.19.0 for the `.ioc`) |
| Project | `STMBoard2`, BSP-based CubeMX project (`USE_NUCLEO_64`): no `LD2_Pin` / `B1_Pin` macros, so `main.c` defines its own |
| Device (programming) | ST-LINK on the Nucleo, also provides the virtual COM port (VCP) |

### Clock
HSI 16 MHz -> PLL: PLLM /4, PLLN x85, PLLR /2 -> **170 MHz** SYSCLK. AHB, APB1 and APB2 all /1. Flash latency 4, voltage scale 1 boost. All timers below assume 170 MHz.

## 2. Pin map

| Function | Arduino pin | MCU pin | Peripheral | Status |
|---|---|---|---|---|
| Nucleo green LED (LD2) | D13 | PA5 | GPIO | confirmed |
| Nucleo blue button (B1) | none | PC13 | GPIO input | confirmed: reads HIGH when pressed |
| Serial TX / RX (ST-LINK VCP) | none | PA2 / PA3 | LPUART1, AF12 | confirmed |
| Pulse sensor signal | A0 | PA0 | ADC1_IN1 | confirmed (analog) |
| Potentiometer | A1 | PA1 | ADC1_IN2 | confirmed |
| Buzzer | D9 | PC7 | TIM3_CH2, AF2 (PWM) | confirmed |
| LED2 (green) | D6 | PB10 | GPIO output | confirmed, active-high |
| LED3 (yellow) | D7 | PA8 | GPIO output | works; assumed active-high (`LED_ACTIVE_HIGH`) |
| Button LEFT | D2 | PA10 | GPIO input | confirmed: HIGH when pressed |
| Button UP | D5 | PB4 | GPIO input | confirmed: HIGH when pressed |
| Button RIGHT | D4 | PB5 | GPIO input | confirmed: HIGH when pressed |
| Button DOWN | D3 | PB3 | GPIO input | confirmed: HIGH when pressed |

Not the pot: A0 (it is the pulse sensor) and A2 / PA4 (ADC2_IN17, tested).
LED1 (under the pulse sensor) is not on any free pin (see section 6).

### Full Arduino header map (Nucleo-64)

| Pin | MCU | Pin | MCU |
|---|---|---|---|
| D0 | PC5 | D8 | PA9 |
| D1 | PC4 | D9 | PC7 |
| D2 | PA10 | D10 | PB6 |
| D3 | PB3 | D11 | PA7 |
| D4 | PB5 | D12 | PA6 |
| D5 | PB4 | D13 | PA5 |
| D6 | PB10 | D14 | PB9 |
| D7 | PA8 | D15 | PB8 |

Analog: A0 PA0, A1 PA1, A2 PA4, A3 PB0, A4 PC1, A5 PC0.

### Things to know about the G4 pins
- Most pins start in **analog mode** after reset. Set them to input or output in code before reading (this includes PC13).
- PB3 (SWO) and PB4 (NJTRST) are debug-related at reset; setting them as normal GPIO input worked fine for the buttons.
- D13 (PA5) drives the Nucleo's own LD2, so it is not free.

## 3. Pulse sensor (A0) and LED1

- LED1 is an **infrared** emitter. The naked eye sees almost nothing (a faint glow at most); a phone camera shows it as a bright purple-white spot. So "always on but very dim" is normal.
- It is not controlled by any free pin. A sweep of every free pin (HIGH and LOW, 2 s each) never changed it. It is simply always on.
- The sensor is **reflective**: IR goes into the fingertip, part of it reflects back to a detector right next to LED1. Measured behaviour: the signal changes only when the finger covers the LED1 area, not the square chip on top (that chip is probably an amplifier IC, not a detector).
- **Confirmed with the analog finder (all of A0..A5 streamed at once): the bottom-left block (LED1 + square part) is the PPG, and its output is on A0 only.** The 8-pin IC under the OLED outline (R1..R9, C1..C4) is most likely its op-amp stage.
- Tips for a usable signal: cover LED1 and the small part beside it fully, light steady pressure (hard pressure blocks blood flow), keep still, shade from room light (50 Hz flicker).
- Signal path in firmware: 64 ADC conversions averaged per sample, keeps 4 extra bits (16-bit scale, 0..65520), sampled at 500 Hz. Smoothed with a 1/16 exponential filter. Beat detection: adaptive min/max tracker, threshold at 60 % of the range, rising edge counts as a beat, release at 40 %, minimum range of about 5 ADC counts (80 on the 16-bit scale), at least 300 ms between beats (max 200 BPM). BPM is smoothed over beats and drops to "--" after 3 s without a beat.

## 4. Peripheral settings that worked

### Serial (LPUART1)
- 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control.
- PA2 / PA3 as AF12, configured in code (CubeMX default baud is 209700, so set it explicitly).
- Newest firmware sends bytes from the 20 kHz TIM6 interrupt (ring buffer, non-blocking) and receives into a 64 byte ring buffer.

### ADC1
- 12-bit, `ADC_CLOCK_SYNC_PCLK_DIV4`, 47.5 cycle sampling, calibrate once after init, software start, polling.
- ADC1 must be enabled in the `.ioc` and the code regenerated so the HAL ADC driver files exist.
- A0 = channel 1 (pulse), A1 = channel 2 (pot). Channel is selected before each reading.

### Buzzer PWM (TIM3 CH2 on PC7)
- Prescaler 169 -> 1 MHz tick, `ARR = 1000000 / Hz - 1`, `CCR2` = duty.
- Loudest at 50 % duty; duty is the volume. Default volume is 10 %, range 1..50 %.
- The small piezo is loudest at about 2-4 kHz, so every note is multiplied by 4 (two octaves up, `SHIFT`).

### LED brightness (TIM6, 20 kHz interrupt)
- Software PWM: 100 steps per period -> 200 Hz, no visible flicker.
- Per-LED duty from the slider value s (0..100): `duty = s * s / 100` (finer control at the dim end), minimum 1 when s > 0.
- The same interrupt services the serial transmit and receive.

### Tempo
One sixteenth note = 15000 / BPM ms. The pot sets 40..160 BPM, smoothed `(3 * old + new) / 4`, read every 40 ms (8 conversions averaged).

## 5. Serial / PuTTY / browser

### PuTTY
- Connection type **Serial** (Session page), COM5 (check Device Manager), speed **115200**, flow control **None**.
- CubeIDE's built-in Terminal has no serial option; use PuTTY.
- **Only one program can hold the COM port.** Close PuTTY (and the browser page) before flashing.

### Browser pages (Web Serial)
- Open the HTML file in **Chrome or Edge** (Firefox has no serial support). Click Connect, pick the STLink port.
- If the status says "waiting for data", press the black reset button on the Nucleo.

## 6. Programs written so far (in order)

1. **Blink / button / UART echo**: first test of toolchain and serial.
2. **Pot reader**: printed ADC values; found the pot on A1.
3. **Pin finder**: cycled through D pins, found the LEDs and the buzzer.
4. **Tune player**: loops a tune transcribed from a guitar tab (note frequencies plus lengths in sixteenths), pot = tempo, fixed quiet volume, shield LEDs flash with the music at low brightness.
5. **Button finder**: watches all free pins, prints which one changes (idle level taken at startup, so polarity does not matter). Found the four buttons.
6. **Pulse sensor reader**: ADC on A0, beat detection, BPM.
7. **PuTTY heart-rate display**: ANSI screen with live BPM and a scrolling waveform.
8. **LED1 finder**: swept free pins HIGH / LOW to find LED1's control line (none found).
9. **Streaming pulse plot** (`pulse_plot.html`): first browser page.
10. **Debug platform**: `main.c` + `debug.html`, described below. Backup: `Claude/main_debug_platform.c.bak` (must not end in `.c` inside the project, or CubeIDE compiles it too and the link fails with two `main`s).
11. **Analog finder** (`Claude/main_analog_finder.c.bak`): streams A0..A5 at 100 Hz (`A,ms,a0..a5`, each a sum of 16 conversions, 247.5-cycle sampling; A2 uses ADC2_IN17) to `analog_finder.html`. Found that the PPG is on A0.
13. **Board monitor** (current `main.c` + `ppg.html`). This is the PPG monitor below, plus the rest of the shield:
    - **Pot:** read on ADC2_IN2, 25 times a second. PA1 is on both ADC1 and ADC2, and ADC1 is busy with the pulse sensor. It sets the **buzzer volume**: 0..50 % duty, where 50 is loudest. It moves in steps of 2 so pot noise is ignored, and a 30 ms beep confirms each change. It starts at the pot's position with no beep.
    - **Buttons:**
      - UP / DOWN change LED2 brightness by +10 / −10.
      - RIGHT / LEFT change LED3 brightness by +10 / −10.
      - Holding a key repeats it (first after 450 ms, then every 150 ms), with a 150 ms lock-out against bounce.
      - After a change the LED lights for 0.6 s, so you can see the new level even if its mode is off.
      - B1 toggles the beat beep.
    - **LEDs:** each has a mode (off / on / flash on beat) and a brightness. Both start at brightness **10**, which is 1 % duty with the `s*s/100` curve. They are dimmed in software from TIM7 at 20 kHz.
    - **Serial:** TX and RX also run from TIM7 through ring buffers, so the main loop never blocks.
    - **Lines:**
      - `P,ms,raw,filt,thr,bpm10,beat,ibi,q,pol,btn,l2,l3` at 100 per second. `btn` bits: 0 LEFT, 1 DOWN, 2 RIGHT, 3 UP, 4 B1. `l2` / `l3` are whether each LED is lit right now.
      - `S,ms,pot,polmode,beep,m2,m3,s2,s3,vol` at 10 per second. `vol` is 0..50 % duty.
    - **Commands:** `i`, `b`, `2m0/1/2`, `3m0/1/2`, `2:NN`, `3:NN`, one per line.
    - **Timer priorities:** TIM7 is 1 and TIM6 is 2, so LED PWM and serial keep running during the ~100 µs ADC burst.
    - **Page:** heart-rate card, a shield panel (D-pad and B1, pot dial, LED bulbs with mode and brightness controls, buzzer), waveform, raw level, tachogram and an event log. It has a Demo mode.
12. **PPG heart-rate monitor** (`Claude/main_ppg.c.bak`):
    - TIM6 interrupt at 500 Hz: 16 conversions of A0 (247.5-cycle sampling).
    - 10-sample moving average = 20 ms window, which cancels 50 Hz mains flicker and its harmonics. Then decimated to 100 Hz.
    - High-pass 0.5 Hz (1st order, removes DC and drift), then low-pass 4 Hz (2nd-order Butterworth).
    - Polarity chosen automatically (steeper edge = systole = made positive), or forced with `i`.
    - Beat = rising through 50 % of a decaying peak envelope, after dipping below 0, at least 300 ms after the last beat, peak at least `AMP_MIN` = 1.5 counts.
    - BPM = 60000 / median of the last 5 intervals. LED2 flashes on each beat; `b` toggles a beep.
    - Line: `P,ms,raw,filt,thr,bpm10,beat,ibi,q,pol,polmode,beep` (q: 0 no pulse, 1 saturated, 2 searching, 3 good, 4 irregular).
    - The page shows BPM, quality hints, filtered and raw traces, a tachogram, and SDNN / RMSSD. It has a Demo button that works without the board.

## 7. Debug platform (current)

### Firmware behaviour (`main.c`)
- Tune player runs non-blocking (state machine on `HAL_GetTick`), pulse sensor sampled at 500 Hz, pot at 25 Hz, buttons read every loop, telemetry sent 50 times a second.
- **Buttons:** UP / DOWN = volume +5 / -5 %, LEFT = pause / resume, RIGHT = restart tune, B1 = mute. 150 ms lock-out against contact bounce.
- **Pot:** tempo.
- **LEDs:** LED2 (green) flashes with every note, LED3 (yellow) with high notes. Brightness and "keep on" set from the page.

### Pins watched (bit number in the mask)
0 D0, 1 D1, 2 D2 (left), 3 D3 (down), 4 D4 (right), 5 D5 (up), 6 D6 (LED2), 7 D7 (LED3), 8 D8, 9 D9 (buzzer), 10 D10, 11 D11, 12 D12, 13 D13 (LD2), 14 D14, 15 D15, 16 A2, 17 A3, 18 A4, 19 A5, 20 B1.
All inputs have the internal pull-up (so they idle HIGH) except D13 and B1 (no pull). D6 / D7 / D9 are driven by the board; their level is still read back.

### Telemetry line (board -> PC, 50 per second)
`T,ms,pot,bpm,hzOut,baseHz,active,vol,muted,paused,idx,N,led2,led3,mask,raw,avg,thr,pbpm,beat,s2,s3,f2,f3`

| Field | Meaning |
|---|---|
| ms | board uptime in ms |
| pot | 0..4095 |
| bpm | tempo from the pot |
| hzOut / baseHz | output frequency (x4) and original note frequency, 0 when silent |
| active | 1 when the buzzer really sounds (note playing, not muted, not paused) |
| vol | volume, % duty |
| muted / paused | 0 or 1 |
| idx / N | current note number (0-based) / notes in the tune |
| led2 / led3 | LED state from the music |
| mask | 21-bit pin level mask (bit = HIGH) |
| raw / avg / thr | pulse raw, smoothed, threshold (16-bit scale) |
| pbpm | heart rate, 0 = none |
| beat | 1 on the line where a beat was detected |
| s2 / s3 | LED slider values 0..100 |
| f2 / f3 | "keep on" flags |

Lines starting with `#` are comments (startup message).

### Commands (PC -> board, one per line, ending with a newline)
| Command | Action |
|---|---|
| `m` | mute / unmute |
| `p` | pause / resume |
| `+` / `-` | volume +5 / -5 % |
| `r` | restart tune |
| `2:NN` / `3:NN` | LED2 / LED3 brightness, NN = 0..100 |
| `2f1` / `2f0` | keep LED2 on / back to music (same with `3f`) |

### Dashboard (`debug.html`)
- Shield buttons as a D-pad plus B1, lit while pressed.
- Potentiometer value, percent bar, BPM.
- Buzzer state (ACTIVE / silent / MUTED / PAUSED), note name and frequency, volume, tune position strip.
- LED2 green and LED3 yellow with brightness sliders and "keep on" checkboxes.
- Pulse sensor: raw (blue), smoothed (green), threshold (yellow dashed), beat dots (red), BPM and heart, 10 s window.
- All 21 input pins: HIGH / LOW, toggle counter, yellow flash on change. Useful for finding what an unknown part is connected to.
- Event log: notes, button presses, pin changes, heartbeats, commands sent.
- Command buttons: Mute, Pause / resume, Vol -, Vol +, Restart tune.

## 8. Troubleshooting log (things that went wrong and the fixes)

| Problem | Cause / fix |
|---|---|
| `main.h` not found, empty project | Project was created without HAL. Make it with standalone CubeMX (CubeIDE 2.x no longer includes it). |
| `LD2_Pin` / `B1_Pin` undefined | BSP-based project. Define pin macros in `main.c` yourself. |
| UART printed nothing in PuTTY | Connection type was not Serial, baud 9600, flow control XON/XOFF. Use Serial, 115200, None. |
| Pot did not respond on A0 | Pot is on A1 (PA1), A0 is the pulse sensor. |
| Buzzer silent | Wrong pin assumed (D10). It is D9 = PC7 = TIM3_CH2. |
| ADC module missing at build | Enable ADC1 in the `.ioc`, regenerate code. |
| PC13 read wrong | Pins start in analog mode; set as input. Button reads HIGH when pressed. |
| COM port busy when flashing / connecting | Another program (PuTTY, the browser page) holds it. Close it. |
| Browser page "no data" | Wrong firmware on the board, wrong port picked, or board needs a reset press. |
| Pulse signal weak | Cover LED1 and its detector fully, light pressure, shade from room light. |

## 9. Open items

- Confirm LED3 polarity (flip `LED_ACTIVE_HIGH` if it is inverted).
- Check the pulse sensor with a finger properly covering LED1 and tune the thresholds from the plot.
- Find out what the other free pins (D0, D1, D8, D10 to D12, D14, D15, A2 to A5) are connected to, using the pin cards on the dashboard.
- Possible next steps: filtering and heart-rate variability on the pulse signal, button-controlled tune selection, a simple game on the D-pad.
