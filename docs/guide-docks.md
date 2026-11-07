# Docks and displays

This guide is about the docks themselves: how many you have, where they sit, how they hide, and the other things WinDock can put on your desktop: folder windows and standalone widgets. It ends with the presets, ready-made dock setups to start from.

## One dock or several

You can have as many docks as you like. Each dock has its own items, its own place on the screen, its own auto-hide and its own settings window. For example, you can have your apps at the bottom, a small dock with widgets at the top, and a dock on a second display.

What a dock shares with the others is its theme: a new dock's **This dock's theme** (its Appearance tab) reads **Use app default (Dark)**, the theme of App settings > Themes. Pick another theme there to give one dock a look of its own; the **↺** beside it goes back to the app default. See [App defaults and local values](guide-settings.md#app-defaults-and-local-values).

Each dock has a name and a colour. The colour shows in its settings window's title bar and header and in the drawing on App settings > Docks and displays, so you can tell the docks apart.

To add a dock:

1. Open App settings > Docks and displays.
2. Click **Add ▾**.
3. Choose **Dock** for a new dock with the running apps and a clock, **Floating dock** for a bar you can place anywhere, or **Dock from a preset…** to start from a ready-made setup.

To make a copy of a dock, select it and click **Duplicate**. To remove one, select it and click **Remove**.

To open a dock's own settings, right-click that dock and choose **Dock settings…**, or double-click it on Docks and displays.

To find out which window belongs to which dock, click **Flash** in a dock's settings window: the dock shows itself and its border pulses in its colour.

## Placement kinds

The **Kind** setting on a dock's Position tab chooses the basic shape and place:

- **Bar on an edge**: the classic dock along the bottom, top, left or right edge of the screen. It can auto-hide.
- **Floating bar**: a bar anywhere on the screen, not attached to an edge. Drag its background to move it. See [Floating docks](#floating-docks).
- **Half circle**: a half ring standing on an edge, with the items along the arc.
- **Circle**: a full ring anywhere on the screen. Ctrl+drag it to move it. When it auto-hides it shrinks to a small hub that stays on screen; point at the hub to open it again.

Rings have no reflections, no 3D tilt and no titles under the icons. Their **Radius** is 0 (automatic) by default: just large enough for the items. **Turn the icons with the ring** makes the icons point outward.

## Edges, alignment and offset

A bar sits on one edge of its display. Left and right docks stand upright; their icons stack from top to bottom.

To move a dock to another edge, right-click it, open **Position on screen** and choose **Bottom**, **Top**, **Left** or **Right**.

To move a dock by dragging:

1. Hold down **Ctrl**.
2. Drag the dock along its edge. It snaps to the centre and to the ends.
3. To put it on another edge, drag it toward that edge and let go there.

A locked dock does not move. Press Esc while dragging to cancel.

On the Position tab's first card you can also set:

- **Alignment**: Centre, Start (the left or top end) or End (the right or bottom end).
- **Full width**: the bar spans the whole edge, flush with the screen edge, like a taskbar.
- **Reserve screen space** keeps maximized windows from covering a dock with auto-hide off.
- **When the icons do not fit**: a dock with more items than fit can **Scroll** (use the mouse wheel or drag the bar), **Shrink the icons**, or **Clip at the screen's end**.

You can also drag a dock to another place, edge or display on App settings > Docks and displays.

The **Layer and title** card has **Title**, a short text drawn beside the bar.

The numbers are in the **Exact offsets** card, closed until you click its heading (it says "· 1 changed" when one of them is set):

- **Offset**: moves the dock from its aligned place along the edge, in pixels at 100 % scaling.
- **Peek when hidden** keeps a thin strip of a hidden dock visible, so you can see where it is.
- **Maximum length** stops the bar from growing past a length. A longer dock scrolls or shrinks its icons.
- **Position x** and **Position y** place a floating bar or a circle.

## Auto-hide

Auto-hide slides a dock off the screen when you do not need it. Set it from the dock menu (**Auto-hide**) or on the dock's Behaviour tab:

- **Off**: the dock is always shown. It floats over windows; turn on **Reserve screen space** on the Position tab to keep maximized windows out from under it.
- **Always**: the dock hides when the mouse leaves it. To bring it back, push the mouse against the screen edge and hold it there a moment.
- **When a window overlaps**: the dock hides only while the active window covers it, and comes back by itself as soon as it does not. Clicking the desktop or opening Start shows it.

The dock does not hide while one of its menus, popups or previews is open, or while you drag something.

In the **Delays and popup gap** card at the end of the Behaviour tab (closed until you click its heading) you can set how long the dock waits before hiding and before showing again. App settings > Animation sets how it hides: slide and fade, slide, fade or at once.

A floating bar never auto-hides. Its layer and the dashboard hotkey decide when you see it.

## Magnification

Icons grow when the mouse moves over them, like the macOS dock. The app's name appears above the icon you point at.

The settings are on the dock's Appearance tab, in the Size card:

- **Magnification**: how big the icon under the pointer gets. 1.00 turns magnification off.
- **Magnification range (icons)**: how many icons beside the pointer grow too.
- **Bar while magnified**: what the bar does while icons grow.

The choices for **Bar while magnified** are:

- **Grows once and stays** (the default): the bar grows once, when the pointer first reaches something that zooms (an app, a folder, a group or the Recycle Bin), and stays grown until the pointer leaves the dock. The dock does not wobble left and right while you move along it. Pointing at a clock, a separator or another widget that does not zoom does not grow the bar.
- **Follows the pointer**: the bar's ends move as the pointer moves along the icons.
- **Always wide (never grows or shrinks)**: the bar always has room for the magnified icons, so nothing but the icons ever moves.

App settings > Animation sets the magnification falloff (smooth or linear) and the speed of every animation.

## Layers

The layer decides where a dock sits among your windows. Set it on the dock's Position tab:

- **Normal (above windows)**: the dock is above every window. This is the usual choice for a dock you use all the time.
- **On the desktop, only while the desktop is showing**: the dock shows only while the desktop is in front, after Win+D or a click on the wallpaper. It hides as soon as you switch to a window. Use it for a dock of desktop things: folders, files, widgets.
- **On the desktop, under every window**: the dock is always shown, but under every window, like a folder window. You see it wherever no window covers the desktop.

The two desktop layers never cover your windows. To reach them while windows are open, use [the dashboard hotkey](#the-dashboard-hotkey).

Standalone widgets have the same three layers.

## Floating docks

A floating dock is a bar placed anywhere on the screen, not on an edge. It suits a panel of widgets or a dock of folders on the desktop.

To add a floating dock:

1. Open App settings > Docks and displays.
2. Click **Add ▾** and choose **Floating dock**.

A new floating dock sits in the middle of the primary display, on the desktop layer. You can also turn any dock into one: choose **Floating bar** under **Kind** on its Position tab.

To move a floating dock, drag the background of its bar (the ends, the space between the items), not an item. It snaps to the screen edges and to other floating docks. Ctrl+drag works anywhere on the bar. A locked dock does not move.

**Faces** on the Position tab says which edge the floating dock faces: where its icons grow and where its popups open. A floating dock at the top of the screen facing **Top** opens its popups downward.

A floating dock has no auto-hide, no alignment and no full width. Its layer decides when it shows.

## The dashboard hotkey

The dashboard hotkey brings every dock and standalone widget on the desktop layers to the front, above your windows, without minimizing anything. The default hotkey is **Win+Shift+D**.

To use the dashboard:

1. Press **Win+Shift+D**. The desktop docks and widgets come to the front.
2. Use them as usual: click, hover, open popups.
3. Press the hotkey again, press **Esc**, or switch to another window to put them back.

Clicking the wallpaper keeps the dashboard up.

You can also choose **Show dashboard** in the dock menu. To change the hotkey, go to App settings > General > Dashboard, click the hotkey box and press the new combination.

## Full-screen apps and games

A window placed above a full-screen game makes the game leave its full-screen mode. To prevent that, WinDock hides its docks while a full-screen app is in front.

While a full-screen app (a game, a video, a presentation) is in front on a display, every dock, the tray dock and every standalone widget that sits above the windows on that display is hidden. They come back as they were when the app closes or another window comes to the front. Docks and widgets on the desktop layers and folder windows sit under the windows, so they stay.

This is on by default. You find it in App settings > General > Full-screen apps > **Hide the docks while a full-screen app is in front**.

A hidden dock also does not come back from the screen edge while a full-screen app or presentation runs.

## Several displays

Each dock lives on one display. Choose it with **Display** on the dock's Position tab:

- **Primary**: the main display.
- **Display 2**, **Display 3** and so on: the other displays.
- **Every display**: a copy of the dock on each display. A change on one copy, such as a new pin, changes them all.

**Show only the windows on this dock's display** on the Running apps tab makes each dock show only the windows on its own display. Use it together with a dock on each display.

When a display is unplugged, its docks are parked: they keep their settings but have no window. App settings > Docks and displays lists them as "Parked (display not connected)". They come back when the display is connected again.

The docks follow changes of resolution, scaling and layout by themselves.

## Folder windows

A folder window shows a folder's contents in a panel on your desktop, under your windows and above the wallpaper. Use it to keep your desktop tidy: one panel for downloads, one for projects, one for pictures.

To add a folder window:

1. Right-click the dock and choose **Add** > **Folder window on the desktop…**.
2. Pick a folder.

It opens at the pointer. You can also add one on App settings > Docks and displays > **Add ▾** > **Folder window…**.

Working with a folder window:

- Move it by its title bar. Resize it by its edges and corners. It snaps to the screen edges and to other folder windows.
- Double-click the title to collapse it to the title bar, and again to open it.
- Double-click a file to open it. Select with click, Ctrl+click and Shift+click.
- F2 renames, Delete sends to the Recycle Bin, Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste, F5 refreshes.
- Drag files in and out, as with File Explorer.
- Right-click a file for the usual File Explorer menu. Right-click the background or the title for the window's own menu: view, sort, collapse, and **Remove this folder window**.

The window follows the folder: new, changed and deleted files show up by themselves.

**Desktop rules** move files from your desktop into the folder window's folder, for example every picture into a Pictures window. Set them in the folder window's settings. See [Desktop rules](guide-settings.md#desktop-rules).

Its right-click menu has the most used options. All of them are in its settings window: open App settings > Docks and displays, select the folder window and click **Open settings**. See [Folder window settings](guide-settings.md#folder-window-settings).

## Standalone widgets

A standalone widget is a widget on the desktop in a window of its own, not on a dock: an analog clock, the weather or a system monitor, which you move and size freely.

To put a widget on the desktop:

- In a dock's Contents tab, click **Desktop** on a script widget's row in **Add to the dock**.
- Or in App settings > Widget library, select a widget and click **Add to the desktop**.
- Or in App settings > Docks and displays, click **Add ▾** > **Standalone widget** and choose the widget.

The widget appears in the middle of the display, on the desktop under your windows, and its settings window opens.

Working with a standalone widget:

- Drag it to move it. Drag its edges or corners to resize it. It snaps to the screen edges, to floating docks and to other standalone widgets.
- A click without moving goes to the widget, as on a dock.
- Right-click it for its menu: its settings, **Window settings…**, **Lock** / **Unlock**, **Reload widget**, **App settings…** and, last, **Remove from desktop**.

Its settings window has a Content and an Appearance tab (the widget's own settings) and a Placement tab (display, layer, lock, background, and the exact position and size). See [Standalone widget settings](guide-settings.md#standalone-widget-settings).

## Presets

A preset is a ready-made dock setup. It sets everything about a dock at once: where it sits, how it hides, its size, its theme and its items. You can change anything afterwards.

There are three ways to use a preset:

- **The welcome dialog**: on the very first start, WinDock asks you to pick a setup for your first dock. Click **Use this dock**, or **Skip (basic dock)** to take the basic dock. To see it again, click **Show the welcome dialog** in App settings > About, or run `WinDock.exe --welcome`.
- **A new dock from a preset**: App settings > Docks and displays > **Add ▾** > **Dock from a preset…**. Check **Copy my pinned apps from the first dock** to bring your pins along, then click **Add dock**.
- **Turn a dock into a preset**: in the dock's settings window, click **⋯** > **Apply a preset…**. Check **Keep my pinned apps and folders** to keep your apps, folders, groups, separators and the Recycle Bin, then click **Apply**.

The chooser lists the presets at the left. At the right it shows the chosen one's description, a small picture of where the dock sits, how it hides, its items in order, and a strip that draws the bar in the preset's look. Use Up and Down to choose and Enter to apply.

Applying a preset replaces every setting and item of the dock. The dock keeps its name, its colour and its display.

The presets:

- **Basic dock**: your apps, the running ones and a clock. The classic dock at the bottom of the screen. Icons grow a little under the pointer, and the dock slides away while a window covers it.
- **Minimal**: small, flat and out of the way. Icons do not grow and there are no window previews. It hides as soon as the pointer leaves it and comes back when you touch the bottom edge.
- **Modern**: glass, big icons and widgets. Icons grow strongly under the pointer. A Start button sits at the left end; volume, now playing, notifications, the weather and a clock at the right end.
- **Windows and labels**: every window with its title and a picture, like the Windows taskbar. Every open window gets its own place, with its title under it and a picture taken every 5 seconds. For the Aero look of Windows 7, pick Windows 7.
- **Windows 7**: the classic Aero taskbar. A full-width glass bar at the bottom with the Start orb, your apps as taskbar buttons, the tray icons, a two-line clock and Show desktop at the right end. It never hides, and maximized windows stop above it. See [Make it look like Windows 7](#make-it-look-like-windows-7).
- **Sidebar**: a bar along the whole left edge, its items from the top down. Every window has its own row with its title beside the icon. It hides while a window covers it.
- **Floating performance**: system load at the top right. A small floating bar with a processor graph, an overview of processor, memory, graphics, disk and network, the battery and a clock. It lives on the desktop under your windows and is locked so it does not move by accident.
- **Desktop dashboard**: a floating bar in the middle of the desktop, under your windows, with a large analog clock, the weather, today's agenda and your notes. Show the desktop or press the dashboard hotkey to see it.

Floating performance and Desktop dashboard have no place for your pinned apps; the chooser says so.

## Make it look like Windows 7

The **Windows 7** preset turns a dock into the Windows 7 taskbar in one step.

1. Apply it: in App settings > Docks and displays click **Add ▾** > **Dock from a preset…** for a new dock, or in a dock's settings window click **⋯** > **Apply a preset…** to change that dock. Choose **Windows 7** in the list, keep **Keep my pinned apps and folders** checked and click **Apply** (or **Add dock**).
2. In App settings > Animation, under **Launching an app**, choose **None**: Windows 7 icons do not bounce. This setting is for every dock, so the preset cannot set it.
3. If you use several docks, turn off or remove the ones you do not need, so only the taskbar is at the bottom.

What you get:

- A bar along the whole bottom edge, with the **Aero glass** theme: a blue-grey glass tint over the blur, square corners, a glossy sheen and taskbar buttons behind your apps. A running app has a glass frame, the app you are using a brighter one, and an app with two or more windows shows stacked outlines.
- The bar keeps its room: maximized windows stop above it, and it never hides.
- At the left the **Start** orb, then your pinned apps and the running ones as 32 DIP icons. Icons do not grow under the pointer. Hovering an app shows its window previews, even for a single window. A middle click opens a new window of the app.
- At the right the tray icons in one row, the clock (the time over the date, for example 2:05 PM over 11/6/2026) and the **Show desktop** strip, flush with the screen's right end. Click it to show the desktop; click again to bring the windows back.
- Applying the preset turns on the tray takeover: the notification area icons move from the Windows taskbar to the dock. To turn that off, go to App settings > Tray icons.

Tuning it:

- **The glass colour**: App settings > Themes, open the **Bar** card and change **Acrylic tint** (the colour over the blur; its opacity sets how strong the colour is). **Background** is the colour used when transparency effects are off. **Sheen** sets how glossy the bar is. Changes on the Themes page apply to every dock, on top of each dock's own theme (see [Dock theme](guide-settings.md#dock-theme)).
- **The orb**: open the Start item's settings (right-click it, then **Start settings…**). **Icon** switches between the orb, the Windows logo and your own image; **Icon size** 0 lets the orb fill the slot.
- **The clock**: open the clock's settings. **Time format** `%#I:%M %p` writes 2:05 PM, `%H:%M` writes 14:05; **Date format** `%#m/%#d/%Y` writes 11/6/2026, `%#d.%#m.%Y` writes 6.11.2026. **Layout** "Taskbar" writes both lines in the same size.
- **Show desktop**: its settings choose what a click does (show the desktop, or minimize every window) and how wide the strip is.
- **Smaller icons**: the dock's Appearance tab sets the icon size; the tray keeps its own icon size in its settings.

What it does not reproduce:

- Labels beside the icons ("never combine"): the dock writes titles under the icons, not beside them, on a bottom bar.
- Aero Peek: hovering Show desktop does not turn the windows into glass.
- The orb sticking out above the bar's top edge.
- Jump lists on a right-click and the progress bars in taskbar buttons.
- A colour picker with the Windows 7 colour names (Sky, Twilight, …): the colour is the theme's **Acrylic tint**.
- Windows 7 has no line between the Start orb and the apps. The dock draws a faint one between its widgets and its icons; to hide it, set the opacity of **Separator** in the theme's **Bar** card to 0 (this also hides the separators you add).
