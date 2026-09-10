# ELRS Backpack analog retuning

The UART3 reader in `src/driver/esp32.c` feeds MSPv2 bytes to
`esp32_handler_process_byte()`. After CRC validation, `msp_process_packet()`
handles `MSP_SET_BAND_CHAN` (zero-based channel) or `MSP_SET_FREQ` (MHz).

Previously, both analog command handlers beeped and called
`app_switch_to_analog()`. That path powers/configures the RTC6715 (including a
200 ms startup wait), selects and resets analog video through `Source_AV()`,
reconfigures video/OSD/audio, and sleeps for one second. The band/channel command
also stopped DVR, adding a two-second wait if recording. The frequency handler
incorrectly updated `scan.channel`, although the tuner uses
`source.analog_channel`.

Both commands now use `elrs_set_analog_channel()`. While holding `lvgl_mutex`, it
checks for Goggle 2, the selected internal analog receiver, completed startup,
`APP_STATE_VIDEO`, and hardware configured for analog module video
(`SOURCE_MODE_AV`, `av_chid == 1`). A changed channel updates
`source.analog_channel` and calls the existing `RTC6715_SetCH()` directly.
Repeated requests for the same channel do nothing. Live retuning leaves receiver
power, the video pipeline, audio, and DVR running. Video signal lock is not a
prerequisite: tuning away from a missing signal must also be fast.

If analog is selected but those initialization checks fail, the helper stops
DVR and uses the original full initialization path. Normal startup and source
switching still call the unchanged `app_switch_to_analog()`. Commands addressed
to other selected sources retain their existing source routing.

Channel indices outside 0–47 and short channel/frequency payloads are ignored.
Frequency requests use the existing 48-channel table; unsupported frequencies
are ignored, and 5880 MHz resolves to its first entry, F8. Analog channel and
frequency replies now read the same analog setting. Automatic beeps were removed
only from the two ELRS tuning commands, including their digital branches.
Explicit `MSP_SET_BUZZER`, manual controls, and UI/status beeps are unchanged.

The startup waits plausibly allow power/video settling; their original rationale
is not documented in the available history. They remain in the startup path.
The existing tuner driver and its two 10 ms register-write waits are unchanged,
so a live retune retains approximately 20 ms of software waiting plus I/O and
scheduling time. PLL settling, decoder synchronization, VTX switching, and any
PAL/NTSC change can add interruption; hardware measurements are still required.

The helper holds the same mutex as manual tuning, source selection, calibration,
and sleep controls, including while checking/updating the channel. This prevents
those operations from interleaving tuner writes or changing the source between
the check and retune. It introduces no new lock ordering. The fast path uses the
same tuner register writes as manual tuning and makes no power-control changes;
code review identified no added receiver/antenna damage mechanism. This is not
hardware qualification, and the existing driver does not verify PLL lock or
propagate I2C failures. Existing automatic DVR handling may still restart a
recording on a video-format change or stop it after sustained signal loss.

Validation used a local Linux regression harness with UndefinedBehaviorSanitizer,
compiling the actual ELRS parser, analog initialization, and RTC6715 driver with
peripheral calls mocked. It passed all 48 channel/frequency entries, repeated
requests and readback, invalid inputs, startup/menu/sleep fallback, inactive
sources, explicit buzzer behavior, digital command dispatch, and a concurrent
source change while the UI mutex was held. It also verified that normal analog
entry still performs startup and that live retuning retains only the two tuner
write waits without stopping DVR. Hardware timing and recorded-video continuity
still need bench verification, including rapid changes and PAL/NTSC transitions.
