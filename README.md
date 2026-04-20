# toyger-wardrobe

`toyger-wardrobe` is an OpenBSD Motif/X11 launcher. It opens a single borderless, centered, always-on-top window with a scrollable application list loaded from JSON.

## Features

- OpenBSD-focused C++20 implementation
- Motif/X11 GUI only
- Borderless centered window with always-on-top WM hints
- Scrollable list of launchable applications
- One-click launch with direct process execution (`fork` + `setsid` + `execvp`)
- Footer status showing current time, hostname, and last opened application
- No terminal and no shell wrappers for launching

## Configuration Format

The configuration file must be JSON with this shape:

```json
{
  "applications": [
    {
      "name": "XClock",
      "argv": ["/usr/X11R6/bin/xclock"],
      "icon": "information"
    }
  ]
}
```

Application fields:

- `name` (required string, non-empty)
- `icon` (required string): one of `information`, `warning`, `error`, `question`, `working`
- one of:
  - `argv` (required array of strings for direct exec)
  - `command` (optional string, tokenized safely without shell semantics)

If both `argv` and `command` are present, `argv` is used.

## Config Search Order

First readable valid file wins:

1. `--config /path/to/file.json`
2. `$XDG_CONFIG_HOME/toyger-wardrobe/apps.json`
3. `~/.config/toyger-wardrobe/apps.json`
4. `/etc/toyger-wardrobe/apps.json`

## Build

```sh
c++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wno-register \
  toyger-wardrobe.cpp -o toyger-wardrobe \
  -I/usr/local/include -I/usr/X11R6/include \
  -L/usr/local/lib -L/usr/X11R6/lib \
  -lXm -lXt -lX11
```

You can also use:

```sh
./build.sh
```

## Run

```sh
./toyger-wardrobe
```

Or with explicit config:

```sh
./toyger-wardrobe --config /path/to/apps.json
```

## Notes

- Invalid entries in `applications` are skipped with concise stderr logs.
- If no usable config is found, the launcher shows a Motif error dialog and exits non-zero.
- Exec failures show a concise Motif error dialog with the application name and reason.
