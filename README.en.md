# gcalntfy

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/gcalntfy)](https://github.com/aviscaerulea/gcalntfy/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/gcalntfy)](LICENSE)
[![Build](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/gcalntfy/actions/workflows/release.yml)

A lightweight Windows tray app that notifies you with Windows notifications before your Google Calendar events start or when they change.

Its main features are event notifications and the event list opened from the tray icon.

Measured physical memory usage is about 10 MB or less.

![The event list opened from the tray icon](docs/images/event-list.png)

## Features

- Event notifications: polls Google Calendar and notifies you before events start or when they change, via Windows notification and sound
- System tray: view the event list and change settings from the tray icon
  - Past events: display can be toggled on or off
  - Next event: shown in bold (turns red when within the configured time)
  - Browser display: clicking an event or the footer opens it in the browser
  - Notification suppression (stopping notifications): right-clicking an event stops notifications for it and shows it with a strikethrough
- Multiple calendar support: handles events from external calendars alongside your main calendar

### System tray

The tray icon shows a badge in the bottom-right corner when there are events yet to start.

Each item in the event list shows the time remaining until the event starts, in the form "(n hours n minutes from now)". Events starting soon are shown in red, and the next event is shown in bold. Clicking the footer opens today's Google Calendar page.

Right-clicking the tray icon opens the tray menu, which provides various settings.

### Google Tasks notifications

| Type | Supported |
|---|:---:|
| Time-specified, non-recurring task | Yes |
| Recurring task | No |

Recurring tasks never appear in the Google Calendar API's event list, so they cannot be handled at the retrieval stage.
"Focus time (silent mode)" events in the Calendar UI are returned with the same internal type as tasks (focusTime), but are identified and excluded from notification targets.

### Notification timing

| Timing | Windows notification | Sound notification | Condition |
|---|:---:|:---:|---|
| Before an event starts (default 5 minutes) | Yes | Yes | Notified for every event |
| At the notification time set in Google Calendar | Yes | Yes | Only when the event has a popup notification set |
| When a change, cancellation, or addition is found | Yes | No | When the content differs from the previous check (changes affecting only events more than one hour past their start time are not notified) |
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

On launch, the app resides in the system tray, checks Google Calendar regularly, and notifies you of your events.

Only the first launch requires linking your Google account. A Windows notification opens your browser, where you click "Allow".

## Configuration

Place `gcalntfy.toml` in the same folder as the executable. The available settings and their meanings are documented in the comments of that file. (Changes take effect after restarting the app.)

If you also place `gcalntfy.local.toml` alongside it, its entries take priority on a per-key basis. Keeping only the settings you want to change there means you do not have to migrate them when a new version replaces `gcalntfy.toml`.

To play a notification sound, also place `sound.wav` (16-bit PCM WAV) in the same folder as the executable.

## Limitations

Recurring tasks never appear in the Google Calendar API's event list, so they are not notification targets. (Time-specified, non-recurring tasks are supported)

## Build

```shell
task build
```

Requires Visual Studio 2022 or Build Tools (C++20, MSVC). The output is `out/gcalntfy.exe`.

Before building, create a `.env` file and set `GOOGLE_CLIENT_ID` / `GOOGLE_CLIENT_SECRET`.
