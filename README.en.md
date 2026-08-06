# gcalntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)

[![Release](https://img.shields.io/github/v/release/aviscaerulea/gcalntfy)](https://github.com/aviscaerulea/gcalntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/gcalntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml)

A lightweight Windows tray app that notifies you with Windows notifications before your Google Calendar events start or when they change.

Measured physical memory usage is under 10 MB. (May vary depending on the environment and the number of events within the next 48 hours.)

## Features

- Event notifications: polls Google Calendar and notifies you before events start or when they change, via toast notification and sound
- Per-time-slot polling frequency: set the polling frequency for each time slot of the day
- Ducking: mutes the processes you specify while the notification sound plays, then restores them afterward
- System tray: check the event list and access various settings from the tray icon
- Past event display: today's past events remain in the event list in gray, and the display can be toggled on or off from the tray menu
- Multiple calendar support: external calendars can also be polled

### System tray

The tray icon shows a badge in the bottom-right corner when upcoming events exist, and shows the remaining event count on hover.

The event list, shown on left click, lets you open today's Google Calendar page by clicking the footer and toggle notification muting by right-clicking an event.

Each item in the event list shows the time remaining until the event starts, recalculated from the current time each time the list is opened. Events starting in less than `urgent_minutes` (default 60 minutes) are shown in red, and the next event (the first upcoming one) is shown in bold.

Right-clicking the tray icon provides various settings. The popup is displayed flush against the taskbar edge.

### Past event display

Events on the current day whose start time has passed remain in the event list in gray, excluding all-day events, and can be clicked to open the event page. The badge, hover count, and footer count only count upcoming events.

"Show past events" in the tray menu toggles the past-event display, which defaults to on. The setting persists after restart, and turning it off only changes the display — event retrieval and notifications are unaffected.

### Task notifications

Among tasks registered in Google Tasks, time-specified, non-recurring tasks are treated as notification targets just like regular events.

| Type | Supported |
|---|:---:|
| Time-specified, non-recurring task | Yes |
| Recurring task | No |

Recurring tasks never appear in the Google Calendar API's event list, so they cannot be handled at the retrieval stage.
"Focus time (silent mode)" events in the Calendar UI are returned with the same internal type as tasks (focusTime), but are identified and excluded from notification targets.

### Notification timing

| Timing | Toast notification | Sound notification | Condition |
|---|:---:|:---:|---|
| `notify_minutes` before (default 5 minutes) | Yes | Yes | Always notified (baseline) |
| At the timing configured in the event's reminders | Yes | Yes | Only when the event has a popup reminder configured |
| On detecting an event's date/time change, cancellation, or new addition | Yes | No | When a difference from the previous poll is detected (changes affecting only events more than one hour past their start time are generally not notified) |
| After running "Refresh now" | Yes | No | Only when run from the tray menu (shows the count of today's remaining events on success, or the failure reason on failure) |

### Notification muting

Right-click an event in the event list (shown via left-click on the tray icon) to toggle notification muting.

| Item | Behavior |
|---|---|
| Scope | The selected instance only (for recurring events, only the current day's instance is muted; subsequent days notify normally) |
| Muted notifications | Toast and sound notifications at the `notify_minutes` and reminders timings |
| Not muted | Change/cancellation/new-event detection notifications (always notified regardless of the mute setting, since they carry important information) |
| Persistence | Saved to `muted_events.json` next to the executable and persists after restart. Entries for past dates are automatically removed on startup |
| Visual indication | Muted events are shown with strikethrough text |

### Whether sound notifications play

| Condition | Sound notification |
|---|:---:|
| A sound file (`sound.wav`) exists next to the executable | Yes |
| No sound file exists | No |
| "Sound notification" is set to OFF in the tray menu | No |
| "Disable while mic/camera in use" is ON and the mic/camera is in use | No |

## Installation

### Requirements

- Windows 10/11
- OAuth 2.0 authentication with a Google account is required on first launch

### Steps

Extract the zip to any folder and run `gcalntfy.exe`.

Or install via [Scoop](https://scoop.sh/):

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install gcalntfy
```

## Usage

```powershell
gcalntfy
```

On launch, the app resides in the system tray and polls Google Calendar according to your configuration to notify you of events.

On first launch, since there is no access token yet, a toast notification opens your browser. Clicking "Allow" with your Google account completes authentication, and the refresh token is saved to the registry (`HKCU\SOFTWARE\gcalntfy`). No re-authentication is needed on subsequent launches.

## Configuration

Place `gcalntfy.toml` in the same folder as the executable. If you also place `gcalntfy.local.toml` alongside it, its entries take priority on a per-key basis. (Changes take effect after restarting the app.)

To play a notification sound, also place `sound.wav` (16-bit PCM WAV) in the same folder as the executable.

```toml
schedule = [1, 1, 1, 1, 1, 1, 1, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 1, 1]
# Minutes before an event to notify (0-30, default 5)
# notify_minutes = 5
# Threshold in minutes for showing events in red in the event list (default 60, 0 to disable)
# urgent_minutes = 60
# Process names to mute while the notification sound plays (empty array to disable)
# duck_targets = ["chrome.exe", "msedge.exe"]
# Additional calendar IDs to poll (primary is always enabled)
# You can find a calendar ID under Google Calendar "Settings" -> "Integrate calendar"
# ext_calendar_ids = ["abc123@group.calendar.google.com"]

# Guard tone settings (BLE headphone workaround)
[guard]
# tone_ms = 1500           # Guard tone length (ms, applies to both start and end, 0 to disable, default: 1500)

# Loudness normalization settings
[loudness]
# enabled = true           # Enable/disable (default: true)
# target = -16.0           # Target loudness LUFS (default: -16.0)
# peak_ceiling = 0.891     # True peak ceiling (default: 0.891 = -1 dBFS)

# Update check settings
[update]
# enabled = true           # Enable/disable the startup GitHub update check (default: true)
```

## Limitations

Recurring tasks never appear in the Google Calendar API's event list, so they are not notification targets. (Time-specified, non-recurring tasks are supported)

## Build

```shell
task build
```

Requires Visual Studio 2022 or Build Tools (C++20, MSVC). The output is `out/gcalntfy.exe`.

Before building, create a `.env` file and set `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET`.
