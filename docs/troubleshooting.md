# Troubleshooting

This page helps when something does not work as expected. Each section names a problem and what to do about it.

## The taskbar is missing

WinDock hides the Windows taskbar while it runs, and gives it back when it quits. The Start menu still opens with the Windows key.

To see the taskbar while WinDock keeps running:

- Right-click the WinDock icon in the notification area and choose **Show Windows taskbar temporarily** (on the dock: right-click it, then **App** > **Show Windows taskbar temporarily**). The taskbar comes back for this session.
- Or clear **Hide taskbar** in the same menus (or **Hide the Windows taskbar** in App settings > General) to keep it shown for good.

If WinDock is not running and the taskbar is still hidden:

1. Open a command prompt in the folder that holds `WinDock.exe`. Press Win+R, type `cmd` and press Enter, then go to that folder.
2. Run the command below. It works without a running dock.

```
WinDock.exe --restore-taskbar
```

Starting WinDock again also puts the taskbar back in order first.

## WinDock crashed

When WinDock hides the taskbar, it writes a small marker file, `taskbar-hidden`, in `%LocalAppData%\WinDock`. It holds the way the taskbar was before. If WinDock crashes or is ended from Task Manager, that marker stays behind, and the next start (or `WinDock.exe --restore-taskbar`) uses it to restore the taskbar exactly as it was.

After a crash:

1. Start WinDock again. It restores the taskbar from the marker, then starts as usual.
2. If you do not want to start it, run `WinDock.exe --restore-taskbar` instead.

Other things clean up by themselves:

- Space a dock reserved on the screen is given back by Windows.
- If the tray takeover was on, the next start asks every app to show its tray icons again. If some icons are still missing, quit and restart the app that owns them.

If WinDock crashes again at start, see [Reset to defaults](#reset-to-defaults).

## A game leaves full screen

Any window placed above a full-screen game makes the game drop out of its full-screen mode. WinDock avoids that by hiding its docks while a full-screen app is in front.

To make sure this is on:

1. Open App settings > General.
2. In the **Full-screen apps** card, check **Hide the docks while a full-screen app is in front**.

While a game, a video or a presentation fills a display, every dock, the tray dock and every standalone widget above the windows on that display is hidden. They come back when you leave the game.

If a game still leaves full screen, or a dock shows over it:

- Some games run in a window that only looks full screen. Switch the game to its full-screen mode in its own settings.
- Put docks you want to keep on the desktop on a desktop layer (Position tab > **Layer**): they sit under every window, so they never get in a game's way. See [Layers](guide-docks.md#layers).
- Set auto-hide to **Always** or **When a window overlaps** on the dock's Behaviour tab.

## A dock is missing

A dock can be out of sight for several reasons. Open App settings > Docks and displays: every dock is listed there with its edge, display and state.

- **Parked (display not connected)**: the dock lives on a display that is unplugged. Plug the display in again, or open the dock's settings and pick another **Display** on its Position tab.
- **Auto-hide**: the dock is hidden until you push the mouse against its screen edge. Check the dock's edge in the list.
- **Desktop layer**: a dock on a desktop layer shows only while the desktop is in front, or under your windows. Press Win+D, or press the dashboard hotkey (Win+Shift+D).
- **A full-screen app**: the docks hide while a full-screen app is in front, and come back when you leave it.
- **Two docks on the same edge**: they overlap. Drag one to another place in the drawing.

To find a dock, open its settings window (double-click it in the list) and click **Flash**. The dock shows itself and its border pulses in its colour.

## Performance and memory

WinDock costs almost nothing while you do not use it: it does no work while nothing changes. Some features do cost a little:

- **Live thumbnails** use the graphics card while the dock is visible. **Snapshots** cost much less. Change it on the dock's Running apps tab.
- **Notifications** (badges, the notification centre, the Notification bell) check Windows' list every few seconds, a few hundredths of a second of processor time each. Raise **Check for notifications every** in App settings > Notifications, or turn badges off.
- **The System monitor and System overview** widgets take a measurement at the fastest **Update every** any of them asks for. A slower update costs less.
- **Icon effects** (glow, reflection, 3D) use a few more megabytes of memory.

To use less memory, choose **Software** as the **Renderer** in App settings > General and restart WinDock. App settings > About shows the renderer in use.

To show HWiNFO sensors (temperatures, fans, power) in a System monitor:

1. Install [HWiNFO64](https://www.hwinfo.com) and start it.
2. In HWiNFO's settings, turn on **Shared Memory Support**.
3. Leave it running (sensors-only and minimized to the tray is fine).
4. In the System monitor's settings, pick the reading under **Metric**. HWiNFO's readings start with "HWiNFO:".

The free version of HWiNFO shares its sensors for 12 hours after each start. After that the readings show a dash until you restart HWiNFO.

## A widget shows an error

A widget that fails shows its problem on its tile:

- **Error**: the widget hit a mistake. The message says what.
- **Stopped**: the widget took too long and was stopped, so it could not slow down the dock.

To see what happened:

1. Open the **Console**: right-click the dock and choose **Tools** > **Widget console…**, or click **Console…** in the widget's settings window, or use App settings > Widget library > **More ▾** > **Console…**.
2. Find the widget in the list. Its log shows the errors and warnings.

To start the widgets again, right-click the dock and choose **Reload widgets** (or click Reload in the Console).

If a widget you installed has a problem with its description file, the message names the field and how to fix it. Widget authors can check a widget folder from the command line with `WinDock.exe --validate-widget <folder>`. See the [widget developer reference](widgets.md).

## Reset to defaults

You can go back to the defaults one part at a time, or start over completely.

To reset one item (a clock, a weather item, a widget or the tray):

1. Open its settings window.
2. Click **Reset to factory defaults…** at the bottom.
3. Click **Yes** in the question that follows. It says what is reset; your App settings are not changed.

A dock or item setting that can follow an app-wide value (a dock's theme, a feed's reminder, a script widget's place) has a **↺** while it has a value of its own: it goes back to the app default. See [Resetting](guide-settings.md#resetting).

To reset the theme, open App settings > Themes, click **⋯** and choose **Reset all to preset**. Each changed value also has its own **↺** button.

To reset one dock, apply the **Basic dock** preset to it: in its settings window click **⋯** > **Apply a layout or look…**. Clear **Keep my pinned apps and folders** for a clean dock.

To start over completely:

1. Right-click the dock and choose **Quit**.
2. Open the folder `%LocalAppData%\WinDock` (paste it into the File Explorer address bar).
3. Rename `config.json`, for example to `config-old.json`, so you keep a copy.
4. Start WinDock. It creates a new settings file with the defaults and shows the welcome dialog.

If your settings file has a mistake in it, WinDock starts with the defaults and leaves the file as it is, so you can fix it. After you fix it, choose **Tools** > **Reload config** in the dock menu.
