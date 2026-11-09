# Quick start

This page gets you going in about five minutes. It shows you how to start WinDock, what is on the screen, how to pin an app, how to change the dock right where it is, and how to quit and get the Windows taskbar back.

## Start WinDock

To start WinDock:

1. Open the folder that holds `WinDock.exe`.
2. Double-click `WinDock.exe`.
3. On the very first start, **Quick setup** opens. Go through its four steps, or click **Cancel** to keep the basic dock. See [Quick setup](#quick-setup).

Only one copy of WinDock runs at a time. Starting it again does nothing harmful.

You can also start and control WinDock from a command prompt:

```
WinDock.exe                     start the dock
WinDock.exe --settings          open the App settings window
WinDock.exe --welcome           open Quick setup again
WinDock.exe --quit              ask the running dock to quit
WinDock.exe --restore-taskbar   bring the Windows taskbar back
```

## Quick setup

Quick setup sets up your docks in four short steps. Every choice shows on your desktop at once, and nothing is kept until you click **Finish**. Click **Next** to go on, **Back** to go back; you can skip any step.

1. **Your docks**: for each dock, pick a layout, the display and the edge. Click **Add a dock** for a second or third one.
2. **Their looks**: pick a look in the gallery, then set the **Transparency** and the **Corners**. With two or more docks, **Every dock uses the same look** gives them one look.
3. **How it behaves**: **Start WinDock when Windows starts**, **Hide the Windows taskbar**, the first dock's **Auto-hide**, and **Animation** (Full, Reduced or Off).
4. **Done**: click **Finish**, or **Show me around** for three short tips on the dock.

**Cancel** (or Esc) puts everything back as it was. On the very first start, that leaves you the basic dock.

Finish is one change: if you do not like the result, right-click the dock and choose the first entry, **Undo “Quick setup”**.

To run Quick setup again, right-click the dock, open **App** and choose **Quick setup…**, or click **Run quick setup again** in App settings > About.

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

## Customize the dock

You change a dock right on the dock: add, move and remove items, and move the dock itself.

To customize a dock:

1. Right-click an empty part of the dock and choose **Customize this dock**. You can also press and hold an empty part of the bar for a moment.
2. A dashed outline goes around the dock, and a small bar with **Done** appears beside it.
3. Change what you like:
   - Point at an item and click its **×** to remove it.
   - Drag an item to move it.
   - Click a **+** on the dock, or **Add items** on the small bar, to open the drawer beside the dock. See [Add a widget](#add-a-widget).
   - Drag an empty part of the bar to move the whole dock along its edge or away from it. It snaps to the centre.
4. Click **Done**, or press Esc, when you are finished.

Clicking another app or the desktop also ends customizing. Every change applies at once and can be undone.

## Dock settings beside the dock

Each dock has its own settings. They open in a panel right beside the dock, so you see every change on the real dock.

To open them, right-click an empty part of the dock and choose **Dock settings…**. While customizing, click **Settings** on the small bar.

The panel has five tabs:

- **Contents**: what is on the dock, in order, and the drawer to add more.
- **Position**: the edge, the display, the alignment and the distances from the centre and from the screen edge.
- **Look**: the dock's look, the icon size and the magnification.
- **Behaviour**: auto-hide, window previews and what the mouse does.
- **Running apps**: how running apps and their windows show.

The panel has no title bar. Drag its outer edges to make it bigger. Press Esc or click **✕** to close it; it also closes when you switch to another app.

What applies to every dock is in **App settings** (right-click the dock, open **App**, choose **App settings…**). An item such as a clock or the weather has its own settings in its right-click menu, for example **Weather settings…**.

To find a setting quickly, press **Ctrl+F** in any settings window and type what you are looking for. To get help on what you see, press **F1**, or click the small round **?** at the right of a card's heading. See [Getting help](guide-settings.md#getting-help).

## Pick a layout or a look

A layout is a ready-made dock: where it sits, how it hides, its items and its look. A look changes only the colours, the transparency, the corners and the icon effects.

To change a dock to a layout or a look:

1. Open the dock's settings (right-click the dock and choose **Dock settings…**).
2. Click **⋯** at the top right of the panel and choose **Apply a layout or look…**.
3. At the top, choose **Layouts**, **Looks** or **Mine** (the layouts you saved yourself).
4. Pick one at the left. The right side shows what it looks like and what it changes.
5. For a layout, leave **Keep my pinned apps and folders** checked to keep your pins, and check **Keep my current look** to keep your colours.
6. Click **Apply**.
7. The dock shows it at once. Click **Keep** in the bar that appears to keep it, or **Cancel** to go back.

If you do neither, closing the settings cancels the preview. The layouts are described in [Layouts and looks](guide-docks.md#layouts-and-looks).

## Undo

Every change can be undone, wherever you made it: in a settings window, on the dock, or from a menu.

- In any settings window, press **Ctrl+Z** to undo and **Ctrl+Y** to redo.
- On the dock, right-click it: while there is something to undo, the first entry says what, for example **Undo “Icon size 48 → 64”**.
- While customizing, click **Undo** on the small bar.

The top of every settings window, and the small bar while customizing, says **✓ Saved** once your changes are on disk. WinDock remembers your last 50 changes until it quits. See [Undo and Saved](guide-settings.md#undo-and-saved).

## Add a widget

Widgets are small items such as a weather forecast, a volume control or a system monitor. You add them from the drawer: a grid of tiles with a live preview of each widget.

To add a widget:

1. Right-click the dock and choose **Customize this dock**.
2. Click **Add items** on the small bar (or a **+** on the dock). The drawer opens beside the dock.
3. Find the widget: type in the search box, or click **Widgets** to show only the widgets.
4. Click the tile to add it at the end of the dock, or drag it onto the dock to the place you want.
5. Click **Done**.

The same drawer is on the Contents tab of the dock's settings. To put a widget on the desktop instead, drag its tile onto the wallpaper, or click the small monitor on its tile.

To change a widget's settings, right-click it on the dock and choose its settings entry, such as **Weather settings…**. See [Widgets](guide-items.md#widgets).

## Where your settings live

WinDock keeps everything in one file:

```
%LocalAppData%\WinDock\config.json
```

You do not need to edit it: every setting is in the settings windows. If you do edit it by hand, save the file and then right-click the dock and choose **Tools** > **Reload config**.

App settings > About shows the file's full path. Click the path to show the file in File Explorer.

The same folder holds your own themes (`themes`), your saved layouts (`layouts`), widgets and clock faces. To back up WinDock, copy this folder.

## Quit and get the taskbar back

To quit WinDock, right-click the dock and choose **Quit**. The Windows taskbar comes back at once.

To keep WinDock running but see the taskbar for a while, right-click the WinDock icon in the notification area and choose **Show Windows taskbar temporarily** (on the dock it is under **App**). The next time WinDock starts it hides the taskbar again.

If the taskbar does not come back, run this from a command prompt in the WinDock folder:

```
WinDock.exe --restore-taskbar
```

## Next steps

- [Settings guide](guide-settings.md): every settings window, page and card.
- [Docks and displays](guide-docks.md): several docks, moving them, placement, layers, floating docks, layouts and looks.
- [Items and widgets](guide-items.md): customizing a dock, apps, folders, groups, the clock, weather, the tray and widgets.
- [Troubleshooting](troubleshooting.md): what to do when something goes wrong.
