# Settings guide

This guide walks through every settings window of WinDock, page by page and card by card. It says what each setting does and when you would want it.

WinDock has three kinds of settings window:

- **App settings**: what applies to every dock and to WinDock as a whole.
- **Dock settings**: one dock. Every dock has its own window. Standalone widgets and folder windows have one too.
- **Item settings**: one item on a dock, such as a clock, a weather item, a widget, the tray, a folder or a group.

You can have as many of these windows open as you like. Opening one that is already open brings it to the front.

Every change applies at once and is saved within a moment. There is no OK or Apply button. Sliders apply when you let go, number boxes on each arrow click, and text boxes when you press Enter or leave the box.

## Opening the settings

To open the App settings:

- Right-click the WinDock icon in the notification area and choose **App settings…**.
- Or right-click the dock, open **App** and choose **App settings…**.
- Or, in a Dock settings window, click **App settings…** at the top right.
- Or run `WinDock.exe --settings`.

To open a dock's own settings:

- Right-click an empty part of that dock and choose **Dock settings…**.
- Or right-click the WinDock icon in the notification area, open **Docks** and choose the dock.
- Or, in App settings > Docks and displays, double-click the dock.

To open an item's settings:

- Right-click the item on the dock and choose its settings entry. It names the item: **Clock settings…**, **Weather settings…**, **Folder settings…**, **Group settings…**, **Tray settings…** or the widget's name, such as **System monitor settings…**.
- Or select the item on the dock's Contents tab and click its settings button (it has the same name).

Each window's title names what it edits: an item's window reads like "Weather · main" (the item, then the dock). When a dock holds two items with the same name, the second is "Weather 2", unless their places or labels already tell them apart.

The settings windows are normal windows: you can move, resize, maximize and minimize them, and each kind remembers its last size. They follow the Windows light or dark app mode. A dock stays shown while its settings window is the active window, so you can watch your changes.

Keyboard: **Ctrl+Tab** and **Ctrl+Shift+Tab** go to the next or previous page or tab. **Esc** closes the window.

## Finding a setting

You do not need to know where a setting lives. The search finds every setting of every settings window by its name, its description or its current value.

To find a setting:

1. Press **Ctrl+F** in any settings window. You can also start typing in the **Find a setting…** box at the top of the App settings window's page list, or choose **⋯** > **Find a setting…** in a Dock window.
2. Type a few words, for example `hide delay` or `magnification`.
3. Pick a result and press **Enter**, double-click it, or click **Go to**.

WinDock opens the right window and page, scrolls to the setting and flashes it, so you can see where it is.

The results show three columns: the setting, where it is (such as "App › General › System") and its current value.

You can also change many settings right in the search:

- Press **Space** on a check box or an option to switch it.
- Press **Space** on a drop-down, or click its value, to pick another choice.

Text boxes, numbers and sliders open with **Go to**.

The search does not cover the items on a dock (use the Contents tab for those), the folder, group and tray item windows, or the dock menus.

## Getting help

The guides you are reading are also built into WinDock.

To open the Help:

- Press **F1** in any settings window. The Help opens at the part of the guide for the page or tab you are on.
- Click the small round **?** button at the right of a card's heading. The Help opens at that card's section.
- Click **Help** in the App settings window's page list, after About. It opens the quick start.
- Right-click the dock (or the WinDock icon in the notification area) and choose **Help…**.
- In App settings > About, click **Quick start guide**.

The Help window lists the guides at the left and shows the chosen guide at the right. Links inside a guide move between the guides. Web links open in your browser.

## Common settings first

Each page and tab starts with the settings you change most often. On a dock's window that is the theme, the icon size and the magnification (the Appearance tab's first card), the display and the screen edge (Position) and auto-hide (Behaviour).

Fine tuning is folded away in cards whose heading opens them, named for what they hold: **Exact offsets**, **Delays and popup gap**, **Magnification details**, **Motion and timings**, **Anchor and offset**, and the theme value groups. They are closed when a window opens. Click a heading (or press Space or Enter on it) to open or close the card. A heading that ends in "· 2 changed" holds two values that differ from the defaults, so you can see at a glance where something was changed.

The search still finds every setting in a closed card: **Go to** opens the card for you.

## App defaults and local values

A few settings of a dock or an item can either follow an app-wide value or have a value of their own:

- **This dock's theme** (Dock settings > Appearance): **Use app default (Dark)** follows the theme of App settings > Themes, whose name is in brackets. Picking a theme gives the dock its own.
- **Use the default reminder** (App settings > Calendar feeds, a feed's form): the feed's events get the Default reminder of the Display card, whose value is in brackets. Clear it to give the feed a reminder of its own.
- **Place** of a script weather widget (its settings window): without a place of its own it shows its dock's Weather item's place, else the default place; the hint under **Place** says which.

While such a setting has a value of its own, a small **↺** at the end of its row (its tooltip says "Reset to app default") makes it follow the app-wide value again.

Everything else is simply the dock's or the item's own setting: there is no app default behind it.

## Resetting

There are two kinds of reset, and they never touch each other's values:

- **Reset to app default** (the **↺** buttons above, and the button of that name in a widget's footer): drops the dock's or item's own value, so it follows the app-wide one again.
- **Reset to factory defaults…** (an item's settings window, the last card of a standalone widget's Content tab): puts every setting of that one item back to the values WinDock ships with. It asks first, and says what it resets. App defaults (App settings) are not changed.

On App settings > Themes, the **↺** of a theme value goes back to the theme preset's value instead (its tooltip says "Reset to the preset's value"), and **⋯** > **Reset all to preset** does that for every value.

## App settings

The App settings window holds what applies to every dock. Its pages are listed at the left in small groups:

- **General**: General, Docks and displays.
- **Look**: Themes, Animation.
- **Data**: Calendar feeds, Notifications, Tray icons.
- **Widgets**: Widget library.
- Then About and Help.

Use the Up and Down keys in the page list to move between pages.

Nothing on these pages changes one dock only. A single dock's settings are in that dock's own window.

### General

The General page has the settings for WinDock itself. The line under the title reminds you where each dock's own settings are.

#### System

- **Start with Windows**: starts WinDock when you sign in to Windows.
- **Hide the Windows taskbar**: hides the Windows taskbar while WinDock runs. Clear it to see the taskbar and the dock at the same time.
- **Renderer**: **Automatic** draws with the graphics card and falls back to software if that fails. **Software** uses no graphics card and a little less memory. A change takes effect after WinDock restarts.

#### Full-screen apps

- **Hide the docks while a full-screen app is in front**: on by default.

While a full-screen app (a game, a video, a presentation) is in front on a display, the docks, the tray dock and the standalone widgets that sit above windows on that display are hidden. They come back as soon as the app closes or another window comes to the front.

Use it to keep games in their real full-screen mode: a window placed above a full-screen game makes the game leave full-screen mode. Docks on the desktop layers and folder windows sit under your windows, so they stay.

#### Dashboard

The dashboard hotkey brings every dock and standalone widget that lives on the desktop to the front, above your windows. See [The dashboard hotkey](guide-docks.md#the-dashboard-hotkey).

- **Hotkey**: click the box and press the combination you want (Win, Ctrl, Alt, Shift and a key). Backspace or Delete clears it. The default is Win+Shift+D.
- The line under the box says whether the hotkey works. If another app already uses that combination, choose another one.
- **Show the dashboard now**: shows the dashboard at once, so you can try it.

### Docks and displays

This page shows all your docks on all your displays, and adds, copies and removes them.

The drawing shows every display to scale, with its usable area shaded. Each dock is drawn on its edge in its own colour, with its name. Floating docks, folder windows and standalone widgets are drawn where they are.

- Click a dock to select it.
- Double-click a dock to open its settings window.
- Drag a dock along its edge, onto another edge or onto another display. It moves when you let go. Press Esc to cancel.

The toolbar under the drawing has:

- **Add ▾**: a menu to add a **Dock**, a **Floating dock**, a **Dock from a preset…**, a **Folder window…** or a **Standalone widget** (a submenu of your widgets).
- A line that describes the selected dock (while you drag: where it will go).
- **Lock all docks**: checked while every dock and standalone widget is locked. Check it to lock them all, clear it to unlock them all.

The list shows every dock, folder window and standalone widget with its edge, its display and whether it is shown, parked or disabled. A dock is "Parked" when its display is not connected; it comes back when the display does. "Disabled" means you turned it off for now (see **Disable** below).

Under the list, for the selected row:

- **Open settings**: opens its own settings window (a double-click on the row does the same).
- **Duplicate**: makes a copy of the dock.
- **Remove**: removes it. You cannot remove the last dock.
- **Disable** / **Enable**: turns it off for a while without removing it, or back on. A disabled dock, folder window or standalone widget has no window, but keeps all its settings and items, so you can bring it back later exactly as it was. One dock always stays on: the button is greyed for the last one.

To add a dock from a preset:

1. Click **Add ▾** and choose **Dock from a preset…**.
2. Pick a preset at the left.
3. Leave **Copy my pinned apps from the first dock** checked to bring your pins along.
4. Click **Add dock**.

### Themes

The Themes page sets the look of every dock: colours, corners, the name label, the dots, the clock and the icon effects. A preview strip under the page shows the result. Move the mouse over it to try the magnification.

#### Choosing a theme

- **Theme**: pick one of the built-in themes (Dark, Light, Glass, Solid, Glass shelf, 3D shelf, High contrast, Aero glass) or one of your own theme files.

**Aero glass** is the look of the Windows 7 taskbar: a blue-grey glass tint over the blur, square corners, a glossy sheen, taskbar buttons instead of the dots, wide gaps between the icons, a thin bar and light name labels. It looks best on a full-width bar with 32 DIP icons and magnification 1.00×. The **Windows 7** dock preset builds the whole taskbar on it: see [Make it look like Windows 7](guide-docks.md#make-it-look-like-windows-7).

The hint under the label counts how many values you changed. Choosing another theme clears your changes.

The **⋯** button next to it has:

- **Export current theme…**: saves the theme with your changes as a theme file you can pick later.
- **Open themes folder**: opens the folder where your theme files live.
- **Reset all to preset**: undoes every change you made on this page.

A single dock can use a different theme: see [Dock theme](#dock-theme). Your changes on this page still apply on top of it.

#### Theme values

Under the Theme card are cards for each part of the theme: Bar, Name label, Indicator dots, Slot buttons, Notification badges, Clock, Placeholder icon, Window previews and Icon effects. They are closed when the window opens. Click a card's heading to open or close it. The heading counts its changed values, for example "Bar · 2 changed".

- A colour has a swatch (click it to pick a colour), its value as `#AARRGGBB` and its opacity from 0 (clear) to 255 (solid).
- On and off values are check boxes; sizes and amounts are number boxes.
- A changed value shows a small **↺** at the end of its row. Click it to go back to the theme's value.

**Bar** also holds the **Sheen** (a glossy highlight over the upper half of the bar), the **Gap between icons** and the **Bar padding** (the space between the icons and the bar's long edges: the bar is the icon size plus twice this).

**Icon effects** holds the glow behind icons (when it shows, its colour, size and strength), the reflection under the icons and the 3D tilt of the bar. Left and right docks have no reflections.

#### Slot buttons

The **Slot buttons** card turns the dots under running apps into taskbar buttons, like the Windows 7 taskbar.

- **Slot style**: **None (indicator dots)** keeps the dots. **Buttons (taskbar style)** draws a frame behind every app, folder, group and Recycle Bin instead. The other rows of the card show only while buttons are on.
- **Running app**, **Active app** and **Attention**: the frame of a running app, of the app you are using, and of an app that wants your attention (that one pulses).
- **Hover**: added while the pointer is over a slot. A pinned app that is not running shows only this.
- **Button border** and **Button corner radius**: the frame's outline and its corners.
- **Light under the pointer (hot tracking)**: a soft light in the icon's colour follows the pointer inside the frame.

An app with two or more windows shows one or two outlines behind its frame, so you can see it is stacked. Wide gaps between the icons leave room for them. The frames do not grow when the icons magnify: use them with magnification 1.00×. A dock shaped as a ring keeps its dots.

### Animation

The Animation page sets how the docks move. A preview strip under it shows the result.

#### Speed

- **Animation speed**: from 0.25× (slow) to 4× (fast). All the way to the left is **Off**: everything moves at once, with no animation.

#### Hiding and showing

How a dock with auto-hide goes away and comes back: **Slide and fade**, **Slide**, **Fade** or **None (instant)**.

#### Launching an app

What the icon does while an app starts: **Bounce until the app opens**, **Hop once**, **Pulse** or **None**.

#### Accessibility

- **Reduce motion**: **Follow Windows** uses the Windows setting (Settings > Accessibility > Visual effects > Animation effects). **On** turns off the slides, bounces and fades. **Off** always animates.

The icons still follow the mouse smoothly when motion is reduced.

#### Motion and timings

This card is closed when the window opens. Click its heading to open it. The heading counts the values you changed, for example "Motion and timings · 1 changed".

- **Magnification falloff**: how quickly the icons beside the pointer get smaller. **Smooth (cosine)** or **Linear**.
- **Easing**: the curve of the movements. **Ease out**, **Linear**, or **Spring**, which overshoots a little and settles.
- **Name label fade**: how long the app name takes to fade in and out, in milliseconds. 0 shows it at once.

### Calendar feeds

Calendar feeds put your events into the clock's month calendar, onto the clock as dots, into the Agenda widget and into reminder banners. A feed is a web address in iCal format or an `.ics` file on your PC. See [Calendar and reminders](guide-items.md#calendar-and-reminders).

#### Feeds

The list shows each feed with its address, how often it is checked and its status (when it was updated and how many events it has, or the error).

- The check box in front of a feed turns it on or off.
- **Add feed…**: opens an empty form below.
- **Remove**: removes the selected feed.
- **Refresh now**: checks every feed for changes now.

Double-click a feed to edit it.

#### Adding or editing a feed

The **Add or edit a feed** card opens when you click **Add feed…** or double-click a feed.

- **Name**: the name you want to see.
- **Address or file**: the web address (`https://`, `http://` or `webcal://`) or the full path of an `.ics` file.
- **Colour**: the feed's colour as `#AARRGGBB`.
- **Check for changes every**: minutes between checks of a web feed.
- **Use the default reminder**: on for a new feed. The feed's events get the **Default reminder** of the Display card below; its value is in brackets, for example "Use the default reminder (15 min)". Turn it off to give the feed a reminder of its own.
- **Remind me before events**: shown while the feed has a reminder of its own: minutes before each event to show it. The **↺** at the end of the row goes back to the default reminder.
- **Enabled**: the feed is used.

Then click **Add feed** for a new feed, or **Save changes** for the selected one.

To add a Google calendar:

1. In Google Calendar, open the calendar's settings and copy its "Secret address in iCal format".
2. In WinDock, click **Add feed…**.
3. Paste the address into **Address or file** and give the feed a name.
4. Click **Add feed**.

#### Calendar display

The **Display** card has:

- **Show dots on days with events**: dots under the days of the month calendar, and a dot after the clock's date while today has events.
- **Default reminder**: minutes before each timed event to show a reminder banner, for feeds without their own. 0 means no reminders.

### Notifications

This page sets how WinDock reads the Windows notifications, for the notification centre widget and the badges on app icons. See [Notifications](guide-items.md#notifications).

#### Notification centre

The **Centre** card:

- The notification centre is a widget: drag it onto a dock from that dock's Contents tab.
- **Tray balloons in the centre instead of Windows toasts**: with the tray takeover on (see [Tray icons](#tray-icons)), the pop-up messages of tray icons go to WinDock's list instead of Windows.
- **Clear an app's badge when it is opened from the dock**: clicking an app on the dock marks its notifications as read.
- **Check for notifications every**: seconds between checks. WinDock checks only while badges, the centre or a widget need it.

#### Notification access

The **Access** card shows whether Windows lets WinDock read your notifications. If it does not, click **Open Windows settings** and turn on "Let apps access your notifications".

WinDock has no Do not disturb switch: use the one in the Windows quick settings.

#### Badges per app

The list shows the apps of every dock and every app that has a notification now. Clear an app's check box to never show a badge on it, on any dock.

Whether a dock shows badges at all is set on that dock's Behaviour tab: see [Notification badges](#notification-badges).

### Tray icons

#### Notification area

- **Show the notification area icons in the dock**: WinDock takes over the notification area (the icons at the right of the taskbar) and shows them on a dock. It needs a System tray item on a dock. Clearing it gives every icon back to Windows.
- The line under it reminds you that each tray item's grid and icons are in that item's own settings window.
- **Put the tray in its own dock**: makes a new small dock at the bottom right of the screen that holds only the tray, and turns the takeover on. Move or remove it on [Docks and displays](#docks-and-displays).

See [The tray](guide-items.md#the-tray).

### Widget library

The Widget library lists every installed widget with its name, id, where it comes from (built in or a folder) and its version.

Select a widget to see a live preview and its description. Then:

- **Add to dock**: adds it to a dock. With several docks a small menu asks which one.
- **Add to the desktop**: puts it on the desktop in a window of its own. See [Standalone widgets](guide-docks.md#standalone-widgets).
- **More ▾**: a menu with **Add from folder…** (install a widget from a folder), **Export built-in to folder** (make a copy of a built-in widget you can edit), **Reload widgets**, **Open widgets folder** and **Console…** (each widget's log and errors).

The line under the buttons shows the result of the last action.

To write your own widgets, see the [widget developer reference](widgets.md).

### About

About shows the WinDock version.

#### Configuration

- **Settings are saved to**: the path of your settings file. Click it to show the file in File Explorer.
- **Open config file**: opens the file in your text editor.
- **Reload config**: reads the file again after you edited it by hand.
- **Renderer in use**: whether WinDock draws with the graphics card or in software.
- **Show the welcome dialog**: opens the dialog of the first start again, to pick a preset for your first dock.
- **Quick start guide**: opens the Help at the quick start.

## Dock settings

Each dock has its own settings window. It edits that dock only. The title bar and the window border take the dock's colour, so you can tell the windows of different docks apart.

The window has a header and five tabs: Contents, Position, Appearance, Behaviour and Running apps. Use Left and Right in the tab strip, or Ctrl+Tab, to move between them.

### The header

The header shows the dock's name in its colour and small chips with its edge, its display, how many items it has, whether it is shown or parked, and whether it is locked.

At the right:

- **Flash**: shows the dock and makes its border pulse for a moment, so you can see which dock this window is for.
- **Lock** / **Unlock**: a locked dock cannot be changed by dragging: no reordering, no unpinning by dragging, no moving the dock. The settings window still changes everything.
- **App settings…**: opens the settings that apply to every dock and to WinDock as a whole.
- **⋯**: a menu with **Find a setting…** (Ctrl+F) and **Apply a preset…**.

To turn a dock into a preset:

1. Click **⋯** and choose **Apply a preset…**.
2. Pick a preset at the left.
3. Leave **Keep my pinned apps and folders** checked to keep your apps, folders, groups, separators and the Recycle Bin.
4. Click **Apply**.

Applying a preset replaces all of the dock's settings and items. Its name, colour and display stay. See [Presets](guide-docks.md#presets).

### Contents tab

The Contents tab shows, arranges and edits everything on the dock: add an item from the drawer, move it, change its everyday settings and remove it, all in one place.

**Items on the dock** is a strip with a tile for every item, in the dock's order:

- Click a tile to select it. Drag a tile to move the item.
- Left and Right select the next item. Ctrl+Left and Ctrl+Right move the selected item.
- Delete removes the selected item. Enter or a double-click opens its settings.

Under the strip is the selected item's inspector: its icon, name and details, and these buttons:

- **◀ Move left** and **Move right ▶**: move the item one place.
- **More settings…**: opens the item's own settings window with all of its settings. For an item whose settings are not shown in the inspector (the tray) the button is named for it, for example **Tray settings…**.
- **Customize icon ▾**: for apps, folders, groups and the Recycle Bin: **Choose icon…**, **Choose image…** and **Reset icon**.
- **Remove**: takes the item off the dock.

Under the buttons are the selected item's everyday settings, which you change right there. They are the same settings as in its own window and apply at once; an open settings window of the item shows the change too, and a change made there shows here.

- A folder: **Contents layout** (Automatic, Grid or List).
- A group: its **Name** and **Contents layout**.
- A clock: **Time format**, **Time zone**, **Show the date under the time** and **Label**.
- Weather: the **Place** it shows (choose another one in **More settings…**), **Name on the dock** and **Units**.
- A widget: the few settings its author marked as the common ones (for example the Analog clock's face, second hand, time zone and label). Some appear only while another setting has a certain value, as in the widget's own window.
- Apps, separators, the Recycle Bin, Running apps, the tray and the notification centre have none here.

Below them, for every item, is **Anchor and offset**, closed at first (most items follow the dock's alignment). Click it to open it. Its heading counts what is set for the selected item, for example "Anchor and offset · 1 changed".

- **Position on the dock**: **Follow the dock's alignment**, or give the item a place of its own: **Start (left / top)**, **Centre** or **End (right / bottom)**.
- **Offset**: shifts this item and the ones after it in its group, in pixels.

Use **Position on the dock** to split a dock into parts, for example a Start button at the left end, your apps in the middle and the clock at the right end.

**Add to the dock** at the right is where you add things:

- A search box to find a widget by name.
- **Apps and folders**: **App or shortcut…** and **Folder…** open a file dialog and pin what you choose.
- **Built in**: Running apps, Clock, Weather, System tray, Notification centre, Recycle Bin and Separator.
- **Script widgets**: every installed widget, each with a small live preview.

To add an item, drag its row onto the strip where you want it, or click its **Add** button. A script widget's **Desktop** button puts it on the desktop in its own window instead. Items a dock can have only once say "Already on the dock" while it has one.

### Position tab

The Position tab sets where the dock sits. It shows only the settings that apply to the kind of placement you chose.

#### Placement

- **Kind**: **Bar on an edge** (the classic dock), **Floating bar** (a bar anywhere on the screen), **Half circle** (a ring on an edge) or **Circle** (a ring anywhere). See [Placement kinds](guide-docks.md#placement-kinds).
- **Display**: Primary, another display, or **Every display** (a copy of the dock on each one).

For a bar on an edge:

- **Edge**: Bottom, Top, Left or Right.
- **Alignment**: Centre, Start (left / top) or End (right / bottom).
- **Full width**: the bar spans the whole edge.
- **Reserve screen space**: maximized windows stop at the dock instead of going under it. Only for a dock with auto-hide off.
- **When the icons do not fit**: **Scroll** (use the wheel or drag the bar), **Shrink the icons**, or **Clip at the screen's end**.

For a floating bar:

- **Faces**: the edge it faces: where its icons grow and its popups open.
- **When the icons do not fit**, as above.

For a half circle: **Edge**, **Radius** (0: automatic) and **Turn the icons with the ring**. For a circle: **Radius** and **Turn the icons with the ring**.

The small monitor at the right shows where the dock sits. It follows every change.

#### Layer

The **Layer and title** card:

- **Normal (above windows)**: the dock sits above every window. Auto-hide keeps it out of the way.
- **On the desktop, only while the desktop is showing**: the dock shows only while the desktop is in front (after Win+D or a click on the wallpaper).
- **On the desktop, under every window**: the dock is always on the desktop, under your windows, like a folder window.
- **Title** (bars): a text drawn beside the bar, for example to tell two docks apart.

The dashboard hotkey brings the desktop layers to the front. See [Layers](guide-docks.md#layers).

#### Exact offsets

The exact numbers are in the **Exact offsets** card, closed when the window opens: dragging the dock (Ctrl+drag on an edge) is the usual way to move it. Click its heading to open it. The heading counts the values that differ from a new dock's, for example "Exact offsets · 2 changed". It shows only the rows that apply to the kind of placement:

- **Position x** and **Position y** (a floating bar: its top-left corner; a circle: its centre): from the top-left of the screen. Leave them empty to centre it.
- **Offset** (a bar on an edge): moves the dock along its edge from its aligned place, in pixels at 100 % scaling.
- **Peek when hidden** (a bar on an edge): keeps a thin strip of the hidden dock on screen, in pixels. 0 is off.
- **Peek opacity**: how visible that strip is, in percent.
- **Maximum length** (bars that are not full width): the longest the bar may get, in pixels. 0 means no limit. A longer dock scrolls or shrinks its icons.

A half circle has none of these, so the card is not shown for it.

### Appearance tab

#### Dock theme

The first card, **Theme and size**, holds what you change most often.

- **This dock's theme**: **Use app default** uses the app's theme from App settings > Themes; its name is in brackets, for example "Use app default (Dark)". Pick another theme to give only this dock a different look. While it has one, a **↺** (Reset to app default) at the end of the row goes back to the app's theme.

The theme's colours and effects are on App settings > Themes (the link opens it). They apply to every dock, on top of this dock's theme.

#### Size

- **Icon size**: the size of the icons at rest.
- **Magnification**: how much the icon under the pointer grows. 1.00 turns magnification off; 2.50 is the largest.

The **Magnification details** card under it is closed at first (its heading counts the values you changed):

- **Magnification range (icons)**: how many icons beside the pointer grow too, from 1 to 4.
- **Bar while magnified**: what the bar does while icons grow.

The choices for **Bar while magnified** are:

- **Grows once and stays** (the default): when the pointer first reaches something that zooms (an app, a folder, a group or the Recycle Bin), the bar grows once, evenly at both ends, and stays grown until the pointer leaves the dock. The dock does not wobble left and right, and the widgets at its ends stay put.
- **Follows the pointer**: the bar's ends move with the pointer as you go along the icons, as on a Mac.
- **Always wide (never grows or shrinks)**: the bar always has room for the magnified icons, so it never changes length.

The preview strip under this tab shows the dock with its theme and sizes.

### Behaviour tab

#### Auto-hide

- **Off**: the dock is always shown.
- **Always**: the dock hides when the mouse leaves it. Push the mouse against the screen edge to bring it back.
- **When a window overlaps the dock**: the dock hides only while the active window covers it, and comes back by itself when it does not.

The delays are in the **Delays and popup gap** card at the end of the tab.

A floating bar has no auto-hide. For a floating bar this card shows one line instead of the options: it is shown by its layer and by the dashboard hotkey. To auto-hide it, choose **Bar on an edge** on the Position tab (click the **Position tab** link).

#### Window previews

- **Show window previews for apps with several windows**: rest the mouse on an app with two or more windows to see a card for each window. Click a card to bring that window to the front; click its × to close it.
- **Show window previews for apps with a single window**: also show a preview for apps with one window.

#### Mouse

- **Middle click on an app**: **Start a new instance**, **Minimize its windows**, **Close its windows** or **Nothing**.
- **Mouse wheel over an app**: **Nothing**, or **Switch between its windows**.

#### Notification badges

- **Notification badges on app icons**: shows the number of unread notifications on each app's icon.

The notification centre itself is a widget: add it on the Contents tab. Which apps may show badges is set in App settings > Notifications.

#### Delays and popup gap

The timings, closed when the window opens. Click the heading to open it. It counts the values that differ from a new dock's, for example "Delays and popup gap · 1 changed".

- **Hide delay** and **Reveal delay** (only while auto-hide is on): how long the dock waits before it hides after the mouse leaves, and before it shows again when you touch the edge, in milliseconds.
- **Preview delay** (only while window previews are on): how long the mouse must rest on an app before its previews open, in milliseconds.
- **Popup gap**: the space between the dock and the popups it opens (previews, folders, the calendar, the weather), in pixels.

### Running apps tab

- **Windows**: **Stacked (one slot per app)**, or **One slot per window**, like the Windows taskbar.
- **Show only the windows on this dock's display**: useful with a dock on each display.
- **Titles under icons**: **Off**, **One line** or **Two lines** of the window title (or the app's name) under each icon.
- **Titles on vertical docks**: **Off** or **Beside the icon**, for left and right docks.
- **Window thumbnails**: **Off**, **Live (GPU capture)** or **Snapshots (every N seconds)**. Each window's icon becomes a small picture of the window.
- **Snapshot every**: seconds between pictures (for snapshots).
- **Live frames per second**: the most pictures per second (for live).
- **Tile length**: the length of each picture tile. 0 makes it twice the icon size.

Live thumbnails use the graphics card; snapshots cost almost nothing. See [Running apps](guide-items.md#running-apps).

## Standalone widget settings

A standalone widget is a widget on the desktop in a window of its own. Its settings window has the same kind of header as a dock's (its display, layer, size and lock, with **Flash**, **Lock** and **⋯**) and the tabs **Content**, **Appearance** and **Placement**.

To open it, right-click the widget and choose its settings entry (it opens on Content) or **Window settings…** (it opens on Placement), or double-click it on App settings > Docks and displays.

### Content and Appearance tabs

The widget's own settings, in cards, the same as for a widget on a dock: what it shows and does on **Content**, how it looks on **Appearance**. A widget whose settings are all of one kind has only that tab. See [Widget settings](guide-items.md#widget-settings). The last card of each has **Reset to factory defaults…** and **Console…** (see [Resetting](#resetting)).

### Placement tab

#### Place, layer and lock

- **Display**: the display the widget is on.
- **Layer**: **Normal (above windows)**, **On the desktop, only while the desktop is showing**, or **On the desktop, under every window** (the default).
- **Locked**: the widget can be neither moved nor resized. Unlocked, drag it to move it and drag its edges to resize it.

#### Window look

- **Background**: **None** (the widget over the wallpaper) or **Panel** (the theme's bar background).

#### Exact position and size

Closed at first: dragging the widget and its edges is the usual way to place and size it. Open it to type the numbers.

- **Position x** and **Position y**: its top-left corner, from the top-left of the screen. Leave them empty to centre it.
- **Width** and **Height**: its size. The hint shows the smallest size the widget allows.

## Folder window settings

A folder window shows a folder on your desktop. Its settings window has one page. Its header has only the **⋯** menu. See [Folder windows](guide-docks.md#folder-windows).

### Which folder

The **Folder** card:

- **Folder**: the folder to show. Click **Browse…** to pick it.
- **Title**: the text in the title bar. Leave it empty to use the folder's name.

### Contents of the window

The **Contents** card:

- **Contents layout**: **Automatic** (a grid for up to 48 entries, a list for more), **Grid** or **List**.
- **Icon size**: from 32 to 96.
- **Sort by**: **Name**, **Date modified**, **Type** or **Size**.
- **Show hidden items**: also show hidden files.
- **Open items with a single click**: one click opens instead of two.
- **Collapsed to the title bar**: shows only the title bar. A double-click on the title does the same.

### Look of the window

The **Appearance** card:

- **Opacity**: how solid the panel is, in percent.
- **Title colour**: as `#AARRGGBB`; empty uses the theme's.
- **Lines of name (grid)**: one or two lines for each name.
- **Hide the title**: no title text; a thin strip stays to move the window by.
- **Transparent background**: no panel, just the icons over the wallpaper.
- **When collapsed, expand while the pointer is over it**: a collapsed window opens while the mouse is over it.

### Desktop rules

Desktop rules tidy your desktop: files that match go into this window's folder.

- **Desktop files matching**: file patterns separated by semicolons, for example `*.png;*.jpg;*.gif`.
- **Move them here automatically when they land on the Desktop**: moves matching files as soon as they appear.
- **Sort desktop now**: moves the matching files now.

A move never overwrites a file, and you can undo it with Ctrl+Z in File Explorer.

## Item settings

An item's settings window edits one item on a dock. Each clock, weather item and widget has its own settings, so two clocks can show two time zones.

The header shows the item's icon, its name, the dock it is on (click it to open that dock's window), its place on the dock and a one-line description. The footer says "Changes apply at once" and has **Reset to factory defaults…** (for a widget, clock, weather or tray item) and **Console…** (for a widget). A widget that shows a shared value has **Reset to app default** beside them while it has a value of its own (see [App defaults and local values](#app-defaults-and-local-values)).

Every widget, clock and weather item has the same structure: what it shows and does is on the **Content** tab (places, feeds, metrics, time zones, labels, what a click does), how it looks on the **Appearance** tab (faces, styles, colours, sizes, motion). An item whose settings are all of one kind has one page and no tabs. **Reset to factory defaults…** and **Console…** stay in the footer under both tabs.

What the page holds depends on the item:

- **Clock**: Content has the cards Time (time format, seconds, time zone), Text (the date, a label) and Click (what a click opens); Appearance has Look (the face, **Open faces folder**). See [The clock](guide-items.md#the-clock).
- **Weather**: one page (it is all content): the Place card (find a place, the name on the dock, the status and **Refresh now**) and the Updates card (units, how often it refreshes). See [Weather](guide-items.md#weather).
- **A script widget**: its own settings, in cards, on Content and Appearance as the widget sorts them. See [Widget settings](guide-items.md#widget-settings).
- **System tray**: the cards The grid, Icons in the grid and Takeover. See [The tray](guide-items.md#the-tray).
- **A folder or a group**: how it shows its contents, its icon, and for a group its shortcuts. See [Folders](guide-items.md#folders) and [Groups](guide-items.md#groups).

**Reset to factory defaults…** puts the item back to the settings WinDock ships with, after asking (see [Resetting](#resetting)). A clock or weather item takes the settings a new clock or weather item starts with; a widget takes its own defaults.

If the item moves on the dock, its window follows it. If the item is removed, its window closes.
