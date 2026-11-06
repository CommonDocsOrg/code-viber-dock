# Items and widgets

This guide is about what sits on a dock: apps, folders, groups, the running apps, the clock, the weather, the tray, notifications and widgets. It says how to add each one, what it does and where its settings are.

## What a dock holds

A dock holds a row of items, in the order you choose. An item can be:

- **An app**: a pinned app, shortcut or program.
- **A folder**: opens its contents in a popup.
- **A group**: several shortcuts in one slot, like a folder on a phone.
- **Running apps**: the place where apps that are running but not pinned appear.
- **A separator**: a thin line between parts of the dock.
- **The Recycle Bin**.
- **A clock**, with a month calendar.
- **Weather**, with a forecast.
- **The system tray**: the notification area icons.
- **The notification centre**: a bell with the list of your notifications.
- **A widget**: a small program such as volume, now playing or a system monitor.

You can have as many clocks, weather items and widgets as you like, each with its own settings. A dock has at most one running apps item, one tray and one notification centre.

## Adding items

Everything is added in one place: the **Add to the dock** drawer on the dock's Contents tab.

To add an item:

1. Right-click the dock and choose **Dock settings…**.
2. On the **Contents** tab, find the item in **Add to the dock** at the right. Use the search box to find a widget by name.
3. Drag its row onto the strip of items at the top, where you want it. Or click its **Add** button.

The drawer has three parts:

- **Apps and folders**: **App or shortcut…** and **Folder…** open a file dialog. Pick one or more apps or shortcuts, or a folder.
- **Built in**: Running apps, Clock, Weather, System tray, Notification centre, Recycle Bin and Separator.
- **Script widgets**: every installed widget, each with a small live preview.

There are quicker ways for some items:

- Drag a shortcut, a program or a folder from File Explorer onto the dock.
- Right-click the dock and open **Add**: **Clock**, **Weather**, **Running apps**, **Tray (notify icons)**, **Notification centre** or **Separator**. **Apps, folders and more widgets…** opens the Contents tab, and **Folder window on the desktop…** makes a folder window.

To remove an item, select it on the Contents tab and click **Remove** (or press Delete). A clock, a weather item, a widget, the tray and the notification centre also have **Remove from dock** at the end of their right-click menu. You can also drag a pinned app that is not running up and off the dock.

## Right-click menus

Right-click an item and its menu starts with what the item does, then how it looks, then its settings, and ends with removal. The last entry, **Dock**, holds the dock menu: the same menu you get by right-clicking an empty part of the dock.

- An app: **Open** or **New window**, **Open file location**, **Close** for each window, **Customize icon**, then **Pin to dock** or **Unpin from dock**.
- A folder: **Open in Explorer**, **Show contents**, **Contents layout**, **Customize icon**, **Folder settings…**, then **Unpin from dock**.
- A group: **Show contents**, **Rename…**, **Contents layout**, **Customize icon**, **Group settings…**, then **Ungroup (shortcuts stay pinned)** and **Unpin group and its shortcuts**.
- The weather: **Refresh weather**, **Show forecast**, **Weather settings…**, then **Remove from dock**.
- The clock: **Show calendar**, **Clock settings…**, then **Remove from dock**.
- A widget: its settings (such as **System monitor settings…**), **Reload widget**, then **Remove from dock**.
- A separator: **Remove separator**.

The dock menu is grouped by task: **Dock settings…**, **Add**, **Position on screen**, **Auto-hide** and **Lock dock** for this dock; then **All docks** (the dashboard, lock all), **App** (App settings, the taskbar, Start with Windows), **Tools** (the config file, widgets, Task Manager), **Help…** and **Quit**.

The WinDock icon in the notification area has the app-wide commands: **App settings…**, **Docks** (each dock's settings), **Show dashboard**, **Show Windows taskbar temporarily**, **Hide taskbar**, **Start with Windows**, **Tools**, **Help…** and **Quit**.

To move an item, drag it on the dock, or drag its tile on the Contents tab. To give an item a fixed place, such as the right end, use **Position on the dock** in the Contents tab's inspector.

## Item settings windows

A clock, a weather item, a widget, the tray, a folder and a group each have their own settings window. Two clocks can show two time zones; two weather items can show two cities.

To open an item's settings:

- Right-click the item on the dock and choose its settings entry, named for the item: **Clock settings…**, **Weather settings…**, **Folder settings…**, **Group settings…**, **Tray settings…** or the widget's name.
- Or select its tile on the dock's Contents tab and click the button of the same name, or double-click the tile.

The settings are described in [Item settings](guide-settings.md#item-settings).

## Apps and pins

A pinned app stays on the dock whether it runs or not.

- **Click** an app to start it. If it is running, a click brings its window to the front, and a second click minimizes it. With several windows, clicks cycle through them.
- **Middle-click** starts a new copy of the app. The dock's Behaviour tab can make it minimize or close the app's windows instead.
- **Mouse wheel** over an app can switch between its windows, if you turn that on in the Behaviour tab.
- **Right-click** for **New window**, **Open file location**, **Close** for each window, **Customize icon**, and **Pin to dock** or **Unpin from dock**.
- **Drop files** on an app to open them with it.

While an app starts, its icon bounces until its window appears. A dot under the icon shows that it is running: brighter for the app in front, orange when the app wants your attention.

To give an app its own icon:

1. Right-click the app on the dock.
2. Open **Customize icon** and choose **Choose icon…** to pick an icon from a program or icon file, or **Choose image…** to use a picture.

**Reset icon** goes back to the app's own icon. The same choices are under **Customize icon ▾** on the Contents tab.

If a pinned program or shortcut no longer exists, the dock shows a placeholder icon instead. It never removes the pin by itself.

## Running apps

Apps that are running but not pinned appear where the dock's **Running apps** item is. Without that item, only pinned apps show their windows. You can drag running apps to reorder them for the session, or drop one among the pins to pin it there.

The settings are on the dock's **Running apps** tab. **Windows** chooses between **Stacked (one slot per app)** and **One slot per window**. With one slot per window, every window gets its own icon, like on the Windows taskbar: its name label shows the window title, and a click brings that window to the front. A minimized window keeps its slot.

### Previews

Rest the mouse on an app with two or more windows and a popup opens with a card for each window: its title and a live picture.

- Click a card to bring that window to the front.
- Click the × on a card, or middle-click it, to close that window.
- Move to another app to switch the popup to it.

Turn previews on or off, include apps with a single window, and set the delay on the dock's Behaviour tab under Window previews.

### Titles

**Titles under icons** writes the window title (or the app's name) under each icon, in one or two lines. On a left or right dock, **Titles on vertical docks** puts the title beside the icon. Titles follow the windows as their titles change.

Use titles with **One slot per window** to tell your windows apart at a glance.

### Thumbnails

**Window thumbnails** turns each window's icon into a small picture of the window, with the app's icon in its corner.

- **Live (GPU capture)**: the pictures follow the windows as they change. It uses the graphics card a little, and only while the dock is visible.
- **Snapshots (every N seconds)**: a new picture every few seconds. It costs almost nothing in between.

**Tile length** sets how long each picture is (0: twice the icon size). Videos protected against copying show as black.

## Folders

A folder on the dock opens its contents in a popup, like a stack on a Mac.

To pin a folder, drag it from File Explorer onto the dock, or click **Folder…** in the Contents tab's drawer.

Working with a folder:

- Click the folder to open its popup. Click it again, or press Esc, to close it.
- Click a file to open it. Click a sub-folder to look inside it; the back arrow (or Backspace) goes up again.
- Type the start of a name to select it, and press Enter to open it.
- Drop files on the popup to move or copy them into the folder.
- Middle-click the folder to open it in File Explorer.

The popup shows a grid or a list. To choose, right-click the folder and open **Contents layout**, or open the folder's settings window and set **Show the contents as**: **Automatic** (a grid up to 48 entries, a list for more), **Grid** or **List**.

The folder's settings window has the same view choice and an Icon card to give it a picture of your own.

## Groups

A group puts several shortcuts in one slot. Its icon shows the first four members. A click opens a popup with all of them.

To make a group:

1. Drag an app's icon onto another pinned app or folder.
2. Hold it there for a moment, until the target grows a little.
3. Let go.

To add another app, drag it onto the group. To take one out, drag it from the group's popup back onto the dock, or right-click it in the popup and choose **Remove from group** or **Move to dock**.

Right-click a group for **Show contents**, **Rename…**, **Contents layout**, **Customize icon**, **Group settings…**, **Ungroup (shortcuts stay pinned)** (its shortcuts become pins again) and **Unpin group and its shortcuts**. A group shows a running dot while any of its apps runs.

The group's settings window (right-click > **Group settings…**) has these cards:

- **Group**: its **Name** and **Show the contents as**.
- **Icon**: see below.
- **Shortcuts in the group**: see below.

### Icons

The Icon card of a folder or a group has **Choose icon…**, **Choose image…** and **Reset icon**. Without an icon of your own, a folder shows the folder's icon and a group shows its first four members.

### Shortcuts in a group

The **Shortcuts in the group** card lists the group's members in order:

- **Add app…** and **Add folder…** add a member.
- **Remove** takes the selected member out.
- **Move up** and **Move down** change the order.

When the last member is removed, the group goes away.

## Separators and the Recycle Bin

A **separator** is a thin line between parts of the dock, for example between your apps and your widgets. Add as many as you like. Drag it to move it; right-click it and choose **Remove separator** to take it away.

The **Recycle Bin** shows whether it is empty or full, and its name label says how many items it holds.

- Click it to open the Recycle Bin.
- Drop files on it to delete them (you can undo that in File Explorer).
- Right-click it and choose **Empty Recycle Bin** to empty it.

## The clock

The clock shows the time and, if you like, the date and a label.

- Click the clock to open a month calendar. Use the arrows (or Page Up and Page Down) to change the month; click the month name to go back to today.
- With calendar feeds, the calendar also shows your events. See [Calendar and reminders](#calendar-and-reminders).

Each clock has its own settings window, with two tabs. The **Content** tab has three cards:

- **Time**: the **Time format** (for example `%H:%M` for 14:05 or `%I:%M %p` for 02:05 PM), **Show seconds** and the **Time zone**.
- **Text**: **Show the date under the time** and a **Label**, such as "Tokyo".
- **Click**: **Clicking the clock opens** the month calendar, an app or web address of your choice, or nothing.

The **Appearance** tab has the **Look** card: the **Face**. **Digital (built in)** is the usual look; the others are clock face files. **Open faces folder** opens the folder for your own faces. The clock's colours and font come from the dock's theme.

To show the time in another city:

1. Add a second clock from the Contents tab's drawer.
2. Open its settings.
3. Choose the city's zone in **Time zone** and type the city in **Label**.

For a round clock with hands, use the **Analog clock** widget.

## Weather

The weather item shows the current conditions as a small picture and the temperature. A click opens a forecast: the next 24 hours and the next 7 days. The data comes from Open-Meteo, a free weather service.

To set the place:

1. Click the weather item. Without a place it shows a map pin, and a click opens its settings.
2. In **Find a place**, type the name of your town.
3. Pick it from the list.

The weather settings window has one page (its settings are all content, so it has no tabs) with two cards:

- **Place**: **Find a place**, the chosen **Place**, **Edit coordinates** (to type a latitude and longitude), the **Name on the dock** under the temperature, and the **Status** with **Refresh now**.
- **Updates**: **Units** (metric or imperial) and **Refresh every** (minutes).

Each weather item has its own place, so a dock can show two cities. Right-click a weather item and choose **Refresh weather** to update every place now.

There is also a **Weather (script)** widget that shows the same data, with more choices such as a forecast of several days on the dock.

## Calendar and reminders

WinDock reads calendars in iCal format: a web address, such as the secret address of a Google calendar or a published Outlook calendar, or an `.ics` file on your PC. It cannot sign in to an account.

To add a calendar, go to App settings > Calendar feeds and click **Add feed…**. See [Calendar feeds](guide-settings.md#calendar-feeds).

Your events then show up:

- **In the month calendar**: days with events get coloured dots. Click a day to see its events. Point at an event to see its time, place and description.
- **On the clock**: a small dot after the date while today has events.
- **In the Agenda widget**: the next event, with its time.
- **As reminders**: a banner beside the dock a few minutes before each event, with **Snooze 5 min** and **Dismiss**.

Reminders are off until you set **Default reminder** (or a feed's own reminder) to some minutes. All-day events have no reminder.

## The tray

The tray shows the notification area icons (the small icons at the right of the Windows taskbar) on the dock. It is off at first.

To show the tray icons on a dock:

1. Right-click the dock and choose **Add** > **Tray (notify icons)**. This also turns on the takeover.
2. Or add **System tray** from the Contents tab's drawer, then check **Show the notification area icons in the dock** on App settings > Tray icons.

Clicks on the icons go to their apps as they would on the taskbar: a right-click opens the app's own menu. **Ctrl+right-click** an icon opens WinDock's tray menu: hide it from the grid, pin it to the dock, or open the tray settings.

When there are more icons than cells, the last cell is an arrow that opens a drawer with every icon.

Windows' own items (network, battery, language, its clock and volume) stay with Windows. The Volume, Battery and clock widgets cover them.

To give the tray a small dock of its own at the bottom right, click **Put the tray in its own dock** on App settings > Tray icons.

### The grid

The tray item's settings window starts with **The grid**:

- **Rows**: across the dock, 1 or 2.
- **Columns**: along the dock, up to 20.
- **Icon size**: 16, 20 or 24.

### Icons in the grid

This card lists every icon in the grid's order. Clear an icon's check box to keep it only in the drawer. **Move up** and **Move down** change the order. You can also drag icons within the grid on the dock, or drag one out onto the dock to pin it there as a full-size icon.

### Takeover

**Reset to Explorer** hands every icon back to Windows and turns the takeover off, on every dock. Turn it on again on App settings > Tray icons.

## Notifications

WinDock can read the notifications in the Windows notification centre. Windows must allow it: Settings > Privacy & security > Notifications > "Let apps access your notifications".

The **notification centre** is a bell on the dock with the number of unread notifications. To add it, drag **Notification centre** from the Contents tab's drawer onto the dock.

- Click the bell to see your notifications, newest first.
- Click a notification to open its app.
- Click the × on a notification, or press Delete, to dismiss it. **Clear all** dismisses every one.

**Badges** put the number of unread notifications on each app's icon. Turn them on with **Notification badges on app icons** on the dock's Behaviour tab. To keep an app from ever showing a badge, clear it in App settings > Notifications > Badges per app.

An app's notifications count as read when you dismiss them in Windows, or when you open the app from the dock.

## Widgets

Widgets are small programs that live in a slot of the dock or on the desktop. Each one runs safely on its own: a widget that misbehaves is stopped without affecting the dock.

### Built-in widgets

WinDock comes with these widgets:

- **Analog clock**: a clock with hands and five faces, optionally with a second hand, the date and a time zone. It grows a little while the pointer is over it.
- **Digital clock (script)**: a scripted copy of the dock's own clock.
- **Calendar page**: today's date as a small calendar page. A click opens the month calendar.
- **Agenda**: your next calendar event. A click shows the next events.
- **Weather (script)**: the weather with more choices, such as a forecast of several days on the dock.
- **Volume**: the sound level. The wheel changes it, a middle-click mutes, a click opens a slider and the output devices.
- **Now playing**: the song or video that is playing, with previous, play / pause and next.
- **Notes**: a sticky note. A click opens a small editor.
- **System monitor**: one measurement, such as processor, memory, graphics, disk, network or battery, as a graph, a bar, a dial or a number.
- **System overview**: processor, memory, graphics, disk and network at once.
- **Battery**: the battery level, whether it charges and the time left.
- **Start** and **Search**: open the Start menu and Windows Search.
- **Notification bell**: a bell like the notification centre, with more choices.
- **Hello**: a tiny example widget.

The System monitor can also show any sensor of HWiNFO (temperatures, fans, power) while HWiNFO runs with its shared memory turned on. See [Performance and memory](troubleshooting.md#performance-and-memory).

### Widget settings

Every widget on a dock has its own settings window. To open it, right-click the widget and choose the entry with its name, such as **System monitor settings…**, or select it on the Contents tab and click the button of the same name.

Every widget's window has the same structure, in two tabs:

- **Content**: what the widget shows and does: its place, feeds, metric or data choices, time zone, labels, and what a click or the wheel does.
- **Appearance**: how it looks: its style, face, colours, sizes and motion.

A widget whose settings are all of one kind (Hello, Digital clock, Start, Search) has one page and no tabs. Inside a tab the settings are grouped in cards. Each setting has a short hint under it; rest the mouse on it for the full explanation. Some settings appear only when another one has a certain value.

The footer, under both tabs, has **Reset to factory defaults…** (back to the widget's own defaults, after asking) and **Console…** (the widget's log and errors).

A widget with a place, such as Weather (script), has the same **Find a place** box as the weather item. Without a place of its own it shows its dock's Weather item's place, else the default place; the hint under **Place** says which. Once you choose a place for it, a **↺** beside the place and **Reset to app default** in the footer make it show the shared place again.

### Widgets on the desktop

A widget can also live on the desktop in a window of its own, moved and sized freely. Click **Desktop** on its row in the Contents tab's drawer, or **Add to the desktop** in App settings > Widget library. See [Standalone widgets](guide-docks.md#standalone-widgets).

### Your own widgets

A widget is a folder with a small description file and a JavaScript file. You can write your own, or change a copy of a built-in one, without any tools.

- App settings > Widget library > **More ▾** > **Add from folder…** installs a widget from a folder.
- **Export built-in to folder** makes an editable copy of a built-in widget.
- **Open widgets folder** opens the folder where your widgets live.
- **Console…** shows each widget's log and errors.

Everything about writing widgets is in the [widget developer reference](widgets.md).
