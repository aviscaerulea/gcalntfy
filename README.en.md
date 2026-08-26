# gcalntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/gcalntfy)](https://github.com/aviscaerulea/gcalntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/gcalntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml)

A lightweight resident app that delivers Google Calendar notifications and shows your event list from the Windows tray.

Measured physical memory usage is about 10 MB or less.

A sister tool, [redntfy](https://github.com/aviscaerulea/redntfy), notifies you of updates to your Redmine tickets.

![The event list opened from the tray icon](docs/images/event-list.png)

## Features

- Event notifications: notifies you via Windows notification before events start or when they change (with sound before an event starts)
- System tray: view the event list and change settings from the tray icon
  - Past events: display can be toggled on or off
  - Imminent notification: notifying again just before an event starts can be toggled on or off
  - Next event: shown in bold (turns red when within the configured time)
  - Browser display: clicking an event or the footer opens it in the browser
  - Notification suppression (stopping notifications): right-clicking an event stops notifications for it and shows it with a strikethrough
  - Hover display: hovering the cursor over the tray icon opens the event list automatically
  - Auto start: launching at Windows logon can be toggled on or off
- Multiple calendar support: handles events from external calendars alongside your main calendar

### System tray

The tray icon shows a badge in the bottom-right corner when there are events yet to start today.

Each item in the event list shows the time remaining until the event starts, in the form "(n hours n minutes from now)". Under an hour it is shortened to "(n minutes from now)", and on the exact hour to "(n hours from now)". Events starting soon are shown in red, and the next event is shown in bold. Clicking the footer opens the weekly view of Google Calendar.

Right-clicking the tray icon opens the tray menu, which provides various settings. Toggles in the tray menu are saved to the registry, not written to the configuration file.

### Notification timing

| Timing | Windows notification | Sound notification | Condition |
|---|:---:|:---:|---|
| Before an event starts (default 5 minutes) | Yes | Yes | Notified for every event |
| Just before an event starts (default 60 seconds ahead) | Yes | Yes | Only when "Imminent notification" is ON in the tray menu (OFF by default; the lead time can be adjusted to 0-60 seconds and the sound can be turned off in the configuration) |
| At the notification time set in Google Calendar | Yes | Yes | Only when the individual event has a popup notification set (calendar-wide default notifications are not covered) |
| When a change, cancellation, or addition is found | Yes | No | When an event's start time differs from the previous check, or an event was added or removed (changes affecting only events more than one hour past their start time are not notified) |
| After running "Refresh now" | Yes | No | Only when run from the tray menu (shows the number of events remaining today on success, or the reason on failure) |

Example notification when an event cancellation is found:

![Windows notification shown when an event cancellation is found](docs/images/cancel-toast.png)

Example notification when "Refresh now" succeeds:

![Windows notification shown when "Refresh now" succeeds](docs/images/update-toast.png)

### Whether sound notifications play

| Condition | Sound notification |
|---|:---:|
| A sound file (`sound.wav`) exists next to the executable | Yes |
| No sound file exists | No |
| "Play notification sound" is set to OFF in the tray menu | No |
| "Disable while mic/camera in use" is ON and the mic/camera is in use | No |
| The imminent notification sound is turned off (imminent notification only) | No |

## Installation

### Requirements

- Windows 10/11
- Microsoft Visual C++ Redistributable (x64)
- OAuth 2.0 authentication with a Google account is required on first launch

### Steps

#### From the release ZIP

Download the ZIP from the [releases page](https://github.com/aviscaerulea/gcalntfy/releases/latest). Extract it to any folder, then run `gcalntfy.exe`.

#### From Scoop

Install via [Scoop](https://scoop.sh/):

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install gcalntfy
```

#### Uninstallation

Settings and credentials are stored in the registry under `HKCU\SOFTWARE\gcalntfy`. Delete this key after uninstalling if you no longer need them.

If you uninstalled with auto start left ON, delete the `gcalntfy` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

## Usage

```powershell
gcalntfy
```

On launch, the app resides in the system tray, checks Google Calendar regularly, and notifies you of your events.

Only the first launch requires linking your Google account. A Windows notification opens your browser, where you click "Allow".

## Configuration

Place `gcalntfy.toml` in the same folder as the executable. The available settings and their meanings are documented in the comments of that file. (Changes take effect after restarting the app.)

If you also place `gcalntfy.local.toml` alongside it, its entries take priority on a per-key basis. Keeping only the settings you want to change there means you do not have to migrate them when a new version replaces `gcalntfy.toml`. For array settings, writing an empty array cancels out the value specified in `gcalntfy.toml`.

The notification sound uses the `sound.wav` included in the distribution. To use a different sound, replace that file in the same folder as the executable with a 16-bit PCM WAV.

## Limitations

### Google Tasks notifications

| Type | Supported |
|---|:---:|
| Time-specified, non-recurring task | Yes |
| Recurring task | No |

Recurring tasks never appear in the Google Calendar API's event list, so they cannot be handled at the retrieval stage.
"Focus time (silent mode)" events in the Calendar UI are returned with the same internal type as tasks (focusTime), but are identified and excluded from notification targets.

### Events not handled

- All-day events: not shown in the event list
- Events you declined: excluded at the retrieval stage
- Cancelled events: excluded at the retrieval stage
