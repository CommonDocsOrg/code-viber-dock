# Round 12 revised: customize the dock where it lives

Proposal, November 8, 2026. Planning and concept preview only; no application changes.
Synthesizes the supplied ideas and the existing windock-round12.md. The original plan is preserved.

## Product decision

Make inline dock editing the primary customization flow. Right-click > Customize this dock opens
an anchored editor beside the actual dock. Its Add items grid, selected-item inspector, Dock
settings, and Saved setups share one editing session. The actual dock is the live preview.
The full settings window remains available for app preferences, detailed inspection, accessibility,
and desktops where an anchored editor cannot fit. Both surfaces edit the same models and commands.

This replaces the old plan's optional 12E with the central deliverable. It supersedes the old
restrictions that edit mode cannot contain settings and that per-dock theme overrides cannot
require a schema extension. Width polish and an always-visible detached preview are supporting
work, rather than prerequisites for inline editing.

## 1. Editing session and flyout

- Enter from Customize this dock, its settings header, or the first-run walkthrough. One active
  dock session per process; switching docks commits valid fields, cancels an in-flight drag, closes
  the prior flyout, and selects the new dock. No automatic session on ordinary hover.
- The real dock stays revealed. Pause auto-hide, magnification, launch gestures, widget click
  actions, tray forwarding, grouping-by-hover, and drag-off removal while editing. Data continues
  updating. Moving widget slots must not restart their runtimes.
- Show an identity-colored outline, selected item border, insertion marker, dedicated Move dock
  grip, Add button, Undo/Redo, and Done. Remove is an explicit selected-item action; no tiny
  mandatory hover target. Do not turn every empty pixel into a move gesture.
- The flyout opens on the inward side of the screen edge: above bottom docks, below top docks,
  right of left docks, left of right docks. Floating docks choose the side with usable space.
  Place against the dock's resting bounds, never over its item hit targets. Keep flyout, dock,
  and drag feedback in the same DPI coordinate space; clamp to the owning monitor's work area.
- Prefer approximately 720-880 DIPs wide with a grid plus 280-320 DIP inspector when room allows.
  In constrained work areas show one pane at a time, with Back and a selected-item breadcrumb.
  A detached, resizable editor is available when even that cannot fit. Preserve context on detach.
- Flyout tabs: Add items, Dock settings, Saved setups. Selecting an item opens its Content /
  Appearance / Position inspector. App-wide settings use a clearly scoped App preferences link.
- Done or Escape exits editing; Escape first cancels a drag, then closes a transient chooser, then
  exits. Clicking another application does not discard changes or exit accidentally. No modal
  overlay or screen dimming. Respect existing fullscreen hiding even while editing.
- Editing is temporary and not serialized. A locked dock requires an explicit Unlock and edit
  action; Done restores its prior lock unless Keep unlocked is chosen. Closing the app clears
  edit holds and restores normal taskbar/window behavior through existing recovery paths.

## 2. Direct manipulation

The grid uses live/sample previews, name, and a short description. Search and categories filter
the grid. Drag directly to the real dock; Add to dock is the keyboard/touch alternative. Choose
the insertion location before committing, with a marker that follows the dock's actual geometry.

- A drop commits one add or move command. Pointer motion changes only a transient placeholder;
  no repeated config writes or runtime reconstruction during hover. Escape/outside drop cancels.
- Drag existing items to reorder. Start / Center / End are labeled edit zones backed by existing
  anchors. Offset remains under Position > Exact offset. Expose Move earlier/later and Move to
  section in the inspector. On vertical docks use spatially appropriate Up/Down labels.
- Select a logical config item, not a renderer slot. Running apps and tray each remain one
  structural item even when they produce many slots. Their edit outlines encompass the region.
  Running applications themselves cannot be removed as configuration entries; offer Hide running
  apps or Unpin as appropriate. Pinning a transient app is an explicit command.
- Widget instances keep stable identity across movement. New copies get fresh instance IDs and
  independent settings. A group moves as a unit; editing its members is a deliberate drilldown.
  Ordinary reorder never implicitly creates a group in edit mode.
- Remove affects the selected dock item, never uninstalls a widget package or deletes files.
  Show the removed item's name with Undo. Avoid drag-out deletion in edit mode.
- First delivery supports movement within the current dock and between its anchor zones. Adding
  a desktop widget uses an explicit destination action. Cross-dock instance moves and desktop
  drag-out are a later phase because instance ownership and storage migration need separate work.
- Arc/circle docks use DockGeometry's logical main-axis order and a matching insertion marker;
  no pretending their slots are a horizontal bar. Disconnected displays cancel capture and park
  the editor with its dock. Overflow scrolls near the pointer using existing drag auto-scroll.

## 3. Settings integrated with the dock

Dock settings has Appearance, Position, Behavior, and Identity sections. Each begins with one
plain sentence. Common controls first; advanced cards open on demand with changed counts.

- Appearance: theme gallery, Quick look, icon size, magnification, then raw theme fields.
- Position: display, edge, alignment, along-edge offset, and distance from screen edge. Keep both
  offset controls visible and distinguish their axes. Shape, floating coordinates, radius,
  desktop layer, full width, and reserved space remain explicit when applicable. Verify existing
  edge-gap support; implement it as one schema/geometry change if the current fields cannot
  express it, rather than misusing the along-edge offset.
- Behavior: auto-hide, app activation, previews, grouping, titles/thumbnails, notification badges.
  Running apps' filter/order belongs to the Running apps inspector; window interaction belongs
  here. Explain unavailable choices, e.g. Floating docks do not auto-hide.
- Identity: editable display name separate from immutable internal dock ID, identity color,
  title visibility Never / Always / On hover, and title placement Beside / Above contents.
  Title placement must participate in measure/layout for every shape, edge, DPI, and overflow;
  expose only supported placements in the first delivery. Rename, Duplicate, Disable, and Save
  setup are visible actions. Do not rename IDs that external automation may reference.
- Widget inspector: reuse manifest-generated fields and native clock/folder/group/tray editors,
  with full Content and Appearance settings accessible through the anchored host. No second
  copy of those values or parallel field-specific behavior. Complex group/tray tables can detach
  initially while keeping the selected dock and breadcrumb.

## 4. Theme ownership and Quick look

Each dock either Follows app theme or uses its Own theme. An own theme supports a preset plus
all per-field overrides; global field overrides no longer apply on top of it. Reverting to Follow
is explicit and undoable. Existing users' current resolved appearance must survive migration.

- Preserve missing/unknown theme files and their reference; show a clear fallback state.
- Version the schema. On first migration, resolve each custom dock's old effective theme and
  materialize the necessary differences in its local overrides. Save a pre-migration backup.
  Do not silently flatten every inheriting dock into a custom one.
- Theme resolution is preset -> overrides at the chosen scope. Scope label always says This
  dock or App theme (affects N following docks). Editing a following dock offers Make a local
  copy before writing any field; it never silently changes all docks.
- Theme gallery tiles use SettingsPreview and include user themes. Quick look offers Tint,
  Transparency, Corners, Icon effects, and Indicator dots; all advanced fields stay accessible.
- Derived controls write existing raw fields through one mapping in Theme, not a new appearance
  layer. Show Mixed when raw fields differ. Tint initially affects background/acrylic tint only;
  related label/button colors change only through an explicit coordinated palette operation.
- Transparency maps to documented alpha fields; do not claim to control system backdrop blur
  strength if the backend cannot. Show the acrylic/system-radius limits and fallback result.
- Round/Square options must describe the effective render result. Reflection is unavailable on
  vertical/ring docks. Dots Off must not accidentally disable taskbar-style active indicators.
- Save theme, Rename, Export, Import, and Reset to preset work at the selected scope.

## 5. Undo, live application, and safe previews

- App owns one bounded timeline, up to 50 semantic customization transactions. Capture minimal
  config domain state or domain patches plus stable dock/item IDs, label, and SettingsScope.
  Exclude transient UI state, running-app lists, widget storage, credentials, and unrelated service
  state. A slider drag, text commit, item move, or setup application is one transaction.
- Every editor uses the same commands. Undo/redo updates docks, flyout, detached settings,
  search, and theme galleries. Validate targets that were removed; clear redo on a new edit.
  Ctrl+Z/Y goes to a text control's own undo while typing, app history otherwise.
- Live changes save through the existing atomic, debounced writer. Display Saving / Saved or
  Save failed with Retry and a useful error. Done closes editing; it is not an Apply button.
- Keep a session transaction marker. Revert this editing session reverses only its transactions,
  not an indiscriminate global snapshot that overwrites later unrelated app changes. Where a
  subsequent edit conflicts, identify it and use normal undo rather than silently rolling it back.
- Whole setup preview is a temporary overlay on one target dock: Preview -> Keep or Cancel.
  Cancel/Escape/target deletion cancels it. Disable other mutations during the preview so cancel
  cannot erase unrelated edits. Keep commits one transaction; only Keep triggers persistence.
- Placement edits remain visible and undoable; editor follows the dock and stays reachable. Use
  an explicit Revert last placement action. Remove the old 12-second keep-by-default prompt:
  a countdown that keeps anyway adds uncertainty without recovery. Reserve a recovery timer
  only for changes that could make controls genuinely inaccessible, with explicit semantics.
- Undo is session-only across the process; saved setups provide durable recovery. Undo does not
  promise to reverse external side effects such as file moves, launched applications, or widget
  network writes. Those are not triggered by selecting items during edit mode.

## 6. Save, reuse, and first run

- Theme: styling only. Dock setup: items, placement, behavior, identity/title options, and a
  referenced/bundled theme. Saved desktop layout: several dock/panel/widget configurations.
  Ship single-dock setup first, whole-desktop layouts later.
- Save this dock opens a name and summary, then writes an atomic user preset under WinDock's
  user presets folder. Preserve widget-instance configuration (not live provider state) and offer
  Include pinned app/file paths. Exports warn about private paths/feed URLs they actually contain;
  exclude credentials and widget data by default. Do not serialize the current generated item slots.
- Applying a Look changes appearance only. Applying a setup offers Keep pinned apps and Keep
  current look, a change summary, Preview, and Keep/Cancel. Default is Create a new dock.
- Duplicate creates new internal dock/item identities and separate widget storage; configuration
  is copied while mutable data is excluded unless explicitly supported. Rename affects display
  name only. Cross-machine imports remap absent displays and missing widget packages explicitly.
- Welcome: choose one or more dock roles/setups, choose a theme for each and a display, then
  startup/taskbar preferences and motion (Follow Windows default). Start with Windows and hiding
  the taskbar are visible opt-ins, not side effects of previewing a setup. Finish enters edit mode
  on the primary dock with a single prompt: Drag an item here, then select it to customize.
- Desktop layout saving, bulk import/export, and advanced title layouts are follow-on scope;
  they must not block getting a usable inline editor into users' hands.

## 7. Native architecture

Keep C++20/Win32, Direct2D/DirectComposition, existing renderer default auto, and existing
service ownership. No web view or large UI dependency; the HTML is a design prototype only.

New responsibilities (names provisional):

- DockEditSession, owned by App: active dock ID, stable selected item ID, temporary holds,
  pending drag, preview transaction, undo commands, switching/closing rules.
- DockEditorHost: anchored/detached host for reused setting forms and a grid-based catalog.
  Extract presentation models from Contents rather than embedding that whole SettingsWindow.
  A new HWND is an editor presentation, not another top-level settings domain.
- DockWindow: edit hit-testing, logical item selection, insertion geometry, edit visuals, Move
  grip and temporary input interception; it does not acquire ownership of global config/history.
- Config/App command layer: stable per-item IDs (migration preserves old entries), scoped theme
  overrides, name/title choices, preset serialization, apply/reconcile/save and transaction labels.
- SettingsPreview/WidgetPreview: gallery thumbnails and detached live preview. Cache by package,
  effective settings/theme, DPI, and provider preview stamp; avoid adding continuous preview
  timers. Offscreen tiles do no work. Do not instantiate a full live runtime per gallery cell.

Likely touch points: App.h/.cpp, DockWindow.h/.cpp, DockGeometry.h, DockManager, Config.h/.cpp,
Theme.h/.cpp, SettingsWindow, SettingsForm, SettingsPagesContents, SettingsPagesAppearance,
SettingsSearch, SettingsPreview, WidgetPreview, Presets/PresetDialog, ContextMenu, and CMakeLists.
Split new editor/history code into focused files rather than growing DockWindow.cpp further.
Allocate new control/message/query IDs after inspecting the current tree; never reuse a planned
numeric range or renumber existing automation. Expose the same model commands to automation,
including active edit dock, selected instance, drop target, undo label, and persistence status.

## 8. Delivery sequence and exit criteria

| Phase | Deliverable | Exit criterion |
|---|---|---|
| 12A | Stable item identity, shared commands, history, migration scaffolding | Reorder and undo preserve widget identity/data; no unrelated config rollback |
| 12B | Edit mode, dock-anchored flyout, grid catalog, select/add/remove/reorder | Can build and edit a dock without opening another settings window; cancel and keyboard paths work |
| 12C | Integrated widget forms; Position/Behavior/Identity; adaptive/detached host | Same fields agree in both hosts; dock never covered by its editor; lock and fullscreen rules hold |
| 12D | Own-theme fields, gallery, Quick look, saved themes | Two docks can differ in every appearance field; old configs retain their appearance |
| 12E | Saved dock setups, duplication, Look/setup preview transactions | Save -> change -> restore works; Cancel never persists a setup preview |
| 12F | Welcome, brief dock-based guidance, docs, accessibility and regression | New user creates chosen docks and edits one; taskbar opt-ins respected |
| Later | Desktop layout bundles, cross-dock moves, richer titles | Instance data/display remapping and backward compatibility specified first |

Verification before release: real pointer add/drop/reorder/remove/cancel; keyboard equivalents;
two identical widgets with independent settings; Running/tray multi-slot selection; group and
overflow handling; every edge/shape at 100/150/200% DPI; short work areas; monitor unplug during
drag; auto-hide and fullscreen; theme migration and missing presets; undo across both hosts;
disk-save failures; user setup round trip; startup/taskbar recovery; no new idle timer cost.
Use an isolated dev build, preserve the user's config, follow STATUS testing cautions, and run
appropriate build/docs/widget checks plus focused native geometry/transaction/migration tests.

## Prototype boundary

The accompanying preview demonstrates the actual-dock metaphor, preview-grid drag/drop,
reordering and zones, selection/removal, local quick appearance, title/offset controls, undo/redo,
setup preview/save, and a compact welcome flow. It does not control the real desktop, execute
widgets, or prove native popup anchoring, multi-monitor/DPI behavior, persistence, or performance.
