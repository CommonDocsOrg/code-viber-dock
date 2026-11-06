# Quick start

This page gets you going in about five minutes. It shows you how to start WinDock, what is on the screen, how to pin an app, where the settings are and how to quit and get the Windows taskbar back.

## Start WinDock

To start WinDock:

1. Open the folder that holds `WinDock.exe`.
2. Double-click `WinDock.exe`.
3. On the very first start, the welcome dialog opens. Pick a dock setup and click **Use this dock**, or click **Skip (basic dock)**. See [Pick a preset](#pick-a-preset).

Only one copy of WinDock runs at a time. Starting it again does nothing harmful.

You can also start and control WinDock from a command prompt:

```
WinDock.exe                     start the dock
WinDock.exe --settings          open the App settings window
WinDock.exe --welcome           open the welcome dialog again
WinDock.exe --quit              ask the running dock to quit
WinDock.exe --restore-taskbar   bring the Windows taskbar back
```

To start WinDock every time you sign in, right-click the dock and check **Start with Windows**.

## What you see

WinDock shows a dock: a see-through bar at the bottom of your screen. On it you find:

- Your pinned apps, such as File Explorer.
- The apps that are running but not pinned.
- A small dot under every app that has a window open.
- A clock at the end.

Move the mouse along the dock and the icons grow under the pointer. The app's name appears above the icon you point at.

- Click an app to start it, or to bring its window to the front.
- Rest the mouse on an app with several windows to see a preview of each window.
- Click the clock to open a month calendar.
- Right-click an empty part of the dock for the dock menu. Right-click an item for that item's own menu; its **Dock** submenu holds the dock menu.
- Right-click the WinDock icon in the notification area for the app menu: **App settings…**, your docks' settings, the taskbar and **Quit**.

## The taskbar

While WinDock runs, it hides the Windows taskbar. The Start menu still opens with the Windows key, and shortcuts such as Win+S (search) and Win+N (notifications) still work.

WinDock always gives the taskbar back when it quits. If it ever stays hidden, see [The taskbar is missing](troubleshooting.md#the-taskbar-is-missing).

If you want the taskbar and the dock at the same time, right-click the dock, open **App** and clear **Hide taskbar**.

## Pin an app

A pinned app stays on the dock when it is not running, so you can start it with one click.

To pin an app that is running:

1. Right-click its icon on the dock.
2. Choose **Pin to dock**.

To pin an app or a folder from File Explorer:

1. Drag the shortcut, the `.exe` file or the folder onto the dock.
2. Drop it between two icons, where the thin marker shows.

To move a pinned app, drag its icon to a new place. To unpin it, right-click it and choose **Unpin from dock**.

## Open the settings

WinDock has three kinds of settings window:

- **App settings**: what applies to every dock, such as themes and animation.
- **Dock settings**: one dock: its items, where it sits, how it hides.
- **Item settings**: one item, such as a clock, the weather or a widget.

To open them:

1. Right-click an empty part of the dock.
2. Choose **Dock settings…**, or open **App** and choose **App settings…**.

You can also right-click the WinDock icon in the notification area: it has **App settings…** and, under **Docks**, the settings of each dock. An item's own settings are in its right-click menu, such as **Weather settings…** or **Folder settings…**.

Every change applies at once and is saved by itself. There is no OK or Apply button.

To find a setting quickly, press **Ctrl+F** in any settings window and type what you are looking for.

To get help on what you see, press **F1** in a settings window, or click the small round **?** button at the right of a card's heading. See [Getting help](guide-settings.md#getting-help).

## Pick a preset

A preset is a ready-made dock setup, such as a small flat dock or a taskbar-like dock with window titles. You can pick one at any time.

To change your dock to a preset:

1. Right-click the dock and choose **Dock settings…**.
2. Click **⋯** at the top right of the window.
3. Choose **Apply a preset…**.
4. Pick a preset at the left. The right side shows what it looks like.
5. Leave **Keep my pinned apps and folders** checked to keep your pins.
6. Click **Apply**.

The presets are described in [Presets](guide-docks.md#presets).

## Turn on auto-hide

Auto-hide slides the dock off the screen when you do not need it.

To change auto-hide:

1. Right-click the dock.
2. Open **Auto-hide**.
3. Choose one of these:

- **Off**: the dock is always shown.
- **Always**: the dock hides when the mouse leaves it. Push the mouse against the screen edge to bring it back.
- **When a window overlaps**: the dock hides only while a window covers it, and comes back by itself.

More auto-hide options are on the Behaviour tab of the Dock settings. See [Auto-hide](guide-docks.md#auto-hide).

## Add a widget

Widgets are small items such as a weather forecast, a volume control or a system monitor.

To add a widget:

1. Right-click the dock and choose **Dock settings…**.
2. Stay on the **Contents** tab.
3. Find the widget in **Add to the dock** at the right.
4. Drag it onto the strip of items at the top, or click its **Add** button.

You can also right-click the dock and open **Add**: it adds a clock, the weather and the other built-in items at once, and **Apps, folders and more widgets…** opens the Contents tab.

To change a widget's settings, right-click it on the dock and choose its settings entry, such as **Weather settings…**. See [Widgets](guide-items.md#widgets).

## Where your settings live

WinDock keeps everything in one file:

```
%LocalAppData%\WinDock\config.json
```

You do not need to edit it: every setting is in the settings windows. If you do edit it by hand, save the file and then right-click the dock and choose **Tools** > **Reload config**.

App settings > About shows the file's full path. Click the path to show the file in File Explorer.

The same folder holds your own themes, widgets and clock faces. To back up WinDock, copy this folder.

## Quit and get the taskbar back

To quit WinDock, right-click the dock and choose **Quit**. The Windows taskbar comes back at once.

To keep WinDock running but see the taskbar for a while, right-click the WinDock icon in the notification area and choose **Show Windows taskbar temporarily** (on the dock it is under **App**). The next time WinDock starts it hides the taskbar again.

If the taskbar does not come back, run this from a command prompt in the WinDock folder:

```
WinDock.exe --restore-taskbar
```

## Next steps

- [Settings guide](guide-settings.md): every settings window, page and card.
- [Docks and displays](guide-docks.md): several docks, placement, layers, floating docks and presets.
- [Items and widgets](guide-items.md): apps, folders, groups, the clock, weather, the tray and widgets.
- [Troubleshooting](troubleshooting.md): what to do when something goes wrong.
