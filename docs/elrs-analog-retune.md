# ELRS Backpack analog retuning

## Fixed-delay observation mode

The ELRS settings page now has **Analog delay: Off / 0.5 s / 1 s**. It defaults to Off,
which retains the existing immediate, silent live retune. The selection is saved
as `[elrs] analog_delay` and `analog_delay_ms` through the normal settings
mechanism. An earlier enabled one-second setting remains at one second after
updating; missing or invalid duration values default to 1000 ms.

For the observation test, select **0.5 s** or **1 s**, enable Backpack, and return to live
internal analog video before changing channel on the Pocket. A new accepted
channel request makes one short beep, holds the active channel for the selected
500 or 1000 ms from receipt, then calls the existing fast tuner routine. The channel setting and
GET channel/frequency replies continue to identify the active old channel during
the wait. Neither the video pipeline nor DVR is restarted for this live change.

The pending request uses `CLOCK_MONOTONIC`. The existing main loop checks it
while holding `lvgl_mutex`, nominally once per 5 ms plus loop work. There is no
delay sleep, extra thread or blocking wait. Actual tuner dispatch can be
later than its deadline if the main loop is busy; this is an observation aid,
not a hard real-time synchronization guarantee. Log entries include request
receipt and tuner-dispatch timestamps. The audible beep also has the normal
beeper thread's scheduling latency.

Repeated SETs for the pending target do not restart its deadline or beep again.
A different target replaces the pending request and starts a new
wait at the selected duration. Returning to the active channel cancels the pending change and beeps to
acknowledge that cancellation. Once a target is active, identical requests are
silent. Legacy Backpack messages have no request ID: an old, differently valued
retry cannot be distinguished from a deliberate new selection. Use isolated
changes for the first timing comparisons.

Leaving video, sleeping, manual hardware tuning, receiver power initialization,
or changing the delay setting invalidates pending work. The executor also checks
that Backpack is enabled and the internal analog source is still initialized.
Startup/menu requests use the existing full initialization path; the configured
observation hold applies only to live video. Digital-source behavior is unchanged.

Use a brief fan-cooled, disarmed test at an explicitly selected **25 mW**, with
the Pocket outside the ExpressLRS Tool. PIT can remove video and would confound
this comparison. Observe the time from beep to loss of the old picture, then the
picture when the goggles retune. A long surviving old picture supports holding
the goggles on the old channel during preparation. Loss soon after the beep
shows an early loss of old-channel reception, but does not measure precisely when
the new-channel RF becomes usable. Two independent receivers or RF/video capture
would be needed to separate that interval from goggles recovery.

An initial comparison needs no new firmware: disable only the goggles Backpack,
leave the goggles on the old channel, and change channel on the Pocket. This
reveals old-picture survival after input; the Pocket voice/input is earlier than
ELRS dispatch and is not equivalent to the new mode's receipt beep.

The user reports that, with Backpack disabled and the earlier fast-retune build
still installed, the old-channel picture stays visible about 0.5–1 s before
dropping out. With Backpack enabled, the goggles appear to retune immediately
and show about 0.5 s of static before new video. These are visual estimates from
separate checks, not a shared-clock measurement of VTX mute. The two selectable
delays provide a comparison using a receipt beep; a variable old-signal hold may
ultimately favor a follower after the decoder loss indication is characterized.

## Existing immediate retune

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
frequency replies now read the same analog setting. With Analog delay Off,
automatic beeps remain removed from the two ELRS tuning commands, including
their digital branches.
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

Validation uses `sh tests/run_elrs_retune_test.sh`, a native regression harness
with UndefinedBehaviorSanitizer,
compiling the actual ELRS parser, analog initialization, and RTC6715 driver with
peripheral calls mocked. It passed all 48 channel/frequency entries, repeated
requests and readback, invalid inputs, startup/menu/sleep fallback, inactive
sources, explicit buzzer behavior, digital command dispatch, and a concurrent
source change while the UI mutex was held. It also verified that normal analog
entry still performs startup and that live retuning retains only the two tuner
write waits without stopping DVR. Hardware timing and recorded-video continuity
still need bench verification, including rapid changes and PAL/NTSC transitions.

The same harness now also covers both 500 ms and 1000 ms requests for all 48
channel/frequency entries, repeated commands before the deadline, receipt beeps, active-channel
readbacks during the wait, replacement and cancellation, menu/sleep round trips,
manual retuning, duration changes, source changes, clock failure and crossing a 32-bit millisecond
boundary. It compiles the actual production parser, scheduler, analog entry and
tuner functions with a controlled monotonic clock and mocked hardware. GitHub
Actions runs it before building the firmware image. These checks establish
software behavior; physical timing remains to be tested on the goggles.
