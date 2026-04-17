# Start Here: SunSDR2 DX Setup In ArtemisSDR

This guide is the shortest reliable path to get a SunSDR2 DX working in this ArtemisSDR fork.

## Before You Start

You need:

- a working SunSDR2 DX on the network
- ArtemisSDR built from this SunSDR-enabled fork
- a Windows audio path you intend to use
  - ASIO if available
  - or VAC if you prefer that workflow

Recommended:

- put the PC and the radio on the same local subnet
- use wired Ethernet while bringing the radio up the first time

## 1. Start ArtemisSDR

Launch ArtemisSDR and open:

- `Setup`

## 2. Select The Radio Model

In ArtemisSDR, set the hardware model to:

- `SunSDR2DX`

If ArtemisSDR was previously configured for another radio, close and reopen it after changing the model.

## 3. Open The Hardware Network Page

Go to:

- `Setup -> H/W Select`

On this page:

- make sure your correct NIC is available
- leave discovery broad enough to see the radio
- for first bring-up, using `Any radio(s)` is usually simplest

Recommended starting settings:

- `Via all NICs`
- `Any Subnet` enabled if your radio is not discovered immediately
- `Any radio(s)`
- `Fixed listen port` = `50001`
- `Protocol must match` enabled

## 4. Select The Radio

In the radio list:

- select the SunSDR radio row
- click `Apply` if needed

When connected successfully, the row should show live identity details, including:

- firmware version
- serial number

The title bar should also show the same live radio identity.

## 5. Power The Radio On

Use the normal ArtemisSDR `Power` control.

On a good connection:

- the panadapter should start
- receive audio should come up
- the title bar should show the SunSDR firmware and serial

## 6. Verify Basic Receive

Check these first:

- tune RX1 and confirm panadapter movement
- confirm band changes work
- if you use RX2, enable it and confirm VFO B activity

## 7. Verify Basic Transmit

Before transmitting into a real load/antenna:

- set a safe power level
- use a dummy load if possible
- verify your PTT path and mic/VAC routing

Then test:

- `MOX`
- `TUNE`

## 8. Make The xPA Button Appear

The main-window `xPA` button does **not** appear automatically just because the SunSDR supports PA control.

You must enable at least one external PA transmit pin in:

- `Setup -> OC Control -> HF/VHF/SWL -> Ext PA Control (xPA)`

Minimum working setup:

1. Enable one `TXPA` output pin
2. Set `Transmit Pin Action` to `Mox/Tune/2Tone`
3. Apply the change

After that, the `xPA` button should appear in the main ArtemisSDR UI.

What it means:

- `xPA` is the ArtemisSDR UI gate for external/PA control
- on this SunSDR fork, that UI state now drives the native SunSDR PA control path

## 9. Initial Power Calibration

Do not trust the on-screen TX power meter as the source of truth for SunSDR yet.

For initial calibration:

- use an external wattmeter
- start at low drive
- verify output on each band you care about

The main ArtemisSDR controls that matter are:

- `Drive`
- `Tune`
- `PA Gain By Band`
- band `Offset`
- `Actual Power @ 100% slider`

## 10. If The Radio Is Found But Audio Is Wrong

Check:

- your selected Windows audio device / ASIO device
- VAC configuration if you rely on VAC
- RX sample rate / audio routing settings

For first bring-up, keep the setup simple:

- one receiver
- no diversity
- no unusual VAC chains

## 11. If xPA Does Not Show

Check all of these:

- you are running the `SunSDR2DX` hardware model
- you enabled at least one `TXPA` pin under `Ext PA Control (xPA)`
- the pin action is set to `Mox/Tune/2Tone`
- you applied the setting

If needed:

- close and reopen `Setup`
- power-cycle the radio connection once

## 12. If The Radio Connects But You Do Not See FW/SN

Check:

- you are on this SunSDR-enabled fork
- the radio actually connected
- the top bar and `H/W Select` row refreshed after connect

The live values currently come from the SunSDR native control path during connect, not from static settings.

## Known Current Limits

- diversity is not currently supported on SunSDR2 DX in this fork
- the displayed TX wattmeter is not yet authoritative for SunSDR calibration
- RX band switching works, but the current implementation still uses a SunSDR-specific recovery path behind the scenes
- intermittent raspy TUNE/MOX TX is a known polish item; cycling VAC (Enable VAC off/on from the sidebar) clears it

## Quick Checklist

1. Set hardware model to `SunSDR2DX`
2. Open `Setup -> H/W Select`
3. Select the SunSDR radio
4. Use `Fixed listen port = 50001`
5. Power on
6. Confirm panadapter/audio
7. Confirm title bar shows firmware and serial
8. Enable `xPA` via `Setup -> OC Control -> HF/VHF/SWL -> Ext PA Control (xPA)`
9. Calibrate output with an external wattmeter
