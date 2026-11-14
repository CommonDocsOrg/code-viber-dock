# Checks docs/widgets.md against the widget host's sources: every name the script API binds
# (src/QuickJsEngine.cpp: the native function lists and the prelude's namespaces) is documented in
# its namespace's section, every `ns.name` the page mentions is bound, the exports the engine calls
# are in the Exports table, and the permissions / manifest / settings-schema keys the package code
# accepts (src/WidgetPackage.cpp) are documented. Prints the drift; exit code 1 when there is any.
#   python tools/check_widget_docs.py [repo root]
import os, re, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
src = open(ROOT + r'/src/QuickJsEngine.cpp', encoding='utf-8').read()
docs = open(ROOT + r'/docs/widgets.md', encoding='utf-8').read()

def entries(listname):
    m = re.search(r'const JSCFunctionListEntry ' + listname + r'\[\] = \{(.*?)\n\};', src, re.S)
    if not m:
        sys.exit('no list ' + listname)
    return re.findall(r'JS_C(?:FUNC|GETSET)_(?:MAGIC_)?DEF\("([A-Za-z0-9_]+)"', m.group(1))

api = {}
api['canvas'] = set(entries('kCanvasFuncs')) | {'linearGradient', 'radialGradient'}
api['path'] = set(entries('kPathFuncs'))
api['dock.theme'] = set(entries('kThemeFuncs'))
api['dock'] = set(entries('kDockFuncs'))
api['timer'] = set(entries('kTimerFuncs'))
api['global'] = set(entries('kGlobalFuncs'))
api['system'] = set(entries('kSystemFuncs'))
api['storage'] = set(entries('kStorageFuncs'))
api['shell'] = set(entries('kShellFuncs'))
api['popup'] = set(entries('kPopupFuncs'))
prelude = re.search(r'constexpr char kPrelude\[\] = R"JS\((.*?)\)JS";', src, re.S).group(1)
members = {}  # round 29 phase 29A: the members proper (one per line in the prelude), for windock.d.ts
for ns, body in re.findall(r'g\.([a-z]+) = Object\.freeze\(\{(.*?)\n?\s*\}\);', prelude, re.S):
    names = set(re.findall(r'(?:^|[,{\s])([a-zA-Z]+):', body))
    names = {n for n in names if n not in ('code', 'isDay', 'ink', 'water', 'from', 'to', 'date', 'n', 'type')}
    api[ns] = names
    members[ns] = set(re.findall(r'^\s+([a-zA-Z]+):', body, re.M)) or names
# Members the prelude adds to a native namespace (`g.storage.onChange = ...`).
for ns, name in re.findall(r'\bg\.([a-z]+)\.([a-zA-Z]+) = ', prelude):
    api.setdefault(ns, set()).add(name)

# Sections of the docs: "### canvas" ... up to the next "## " / "### ".
def section(title):
    m = re.search(r'^### ' + re.escape(title) + r'\b.*?$(.*?)(?=^##)', docs, re.S | re.M)
    return m.group(1) if m else ''

where = {'canvas': 'canvas', 'path': 'canvas', 'dock.theme': 'dock', 'dock': 'dock', 'timer': 'timer and redraws',
         'global': 'timer and redraws', 'system': 'system', 'storage': 'storage', 'shell': 'shell', 'popup': 'popup',
         'console': 'console', 'http': 'http', 'widget': 'widget', 'audio': 'audio', 'media': 'media',
         'notifications': 'notifications', 'calendar': 'calendar', 'weather': 'weather', 'perf': 'perf',
         'network': 'network', 'input': 'input',  # round 27
         'notify': 'notify',  # round 28
         'links': 'links', 'settings': 'settings'}  # round 29
# Round 29 phase 29C: settings.set / settings.onChange are defined by the host on every settings
# object from the prelude's n.settingsMethods (not a g.<ns> object).
m_sm = re.search(r'n\.settingsMethods = Object\.freeze\(\{(.*?)\n\s*\}\);', prelude, re.S)
if m_sm:
    api['settings'] = set(re.findall(r'^\s+([a-zA-Z]+):', m_sm.group(1), re.M))
drift = []
for ns, names in sorted(api.items()):
    text = section(where[ns])
    if not text:
        drift.append(f'{ns}: no "### {where[ns]}" section in docs/widgets.md')
        continue
    for name in sorted(names):
        if not re.search(r'(?<![A-Za-z0-9_])' + re.escape(name) + r'(?![A-Za-z0-9_])', text):
            drift.append(f'undocumented: {ns}.{name} (not in "### {where[ns]}")')
# The reverse: `ns.name` in the docs that the bindings do not have.
known_ns = {k for k in api if '.' not in k and k not in ('global', 'path', 'settings')}  # settings.<key>: any setting's value
for ns, name in sorted(set(re.findall(r'`(' + '|'.join(sorted(known_ns)) + r')\.([a-zA-Z0-9]+)', docs))):
    config_keys = {('notifications', 'balloons'), ('weather', 'units'), ('clock', 'face'), ('widget', 'json'),
                   ('system', 'uptime'),  # a perf metric key, not system.uptime
                   ('network', 'up'), ('network', 'down')}  # perf metric keys (round 27: `network` is a namespace now)
    if name not in api[ns] and not (ns == 'dock' and name == 'theme') and (ns, name) not in config_keys:
        drift.append(f'documented but not bound: {ns}.{name}')
pkg = open(ROOT + r'/src/WidgetPackage.cpp', encoding='utf-8').read()
m = re.search(r'kFnNames\[\]\s*=\s*\{(.*?)\};', src, re.S) or re.search(r'const char\* const k[A-Za-z]*Names?\[\][^{]*\{(.*?)\};', src, re.S)
exports = re.findall(r'"([a-zA-Z]+)"', m.group(1)) if m else []
table = re.search(r'^## main\.js(.*?)^## ', docs, re.S | re.M).group(1)
for e in exports:
    if '`' + e + '(' not in table:
        drift.append('export not in the Exports table: ' + e)
perms = set(re.findall(r's == L"([a-z]+)"', pkg[pkg.index('void ReadPermissions'):pkg.index('void ReadSettingsSchema')]))
row = re.search(r'^\| `permissions` \|.*$', docs, re.M).group(0)
for p in sorted(perms):
    if '"' + p + '"' not in row:
        drift.append('permission not in the widget.json table: ' + p)
if 'kTopKeys' in pkg:
    keys = re.findall(r'L"([A-Za-z]+)"', re.search(r'kTopKeys\[\] = \{(.*?)\};', pkg, re.S).group(1))
    wj = re.search(r'^## widget\.json(.*?)^### Settings schema', docs, re.S | re.M).group(1)
    for k in keys:
        if '`' + k + '`' not in wj and '`' + k + '.' not in wj:
            drift.append('manifest key not in the widget.json table: ' + k)
    fk = re.findall(r'L"([A-Za-z]+)"', re.search(r'kFieldKeys\[\] = \{(.*?)\};', pkg, re.S).group(1))
    schema = re.search(r'^### Settings schema(.*?)^## ', docs, re.S | re.M).group(1)
    for k in fk:
        if '`' + k + '`' not in schema:
            drift.append('settings schema key not documented: ' + k)

# Round 29 phase 29A: docs/windock.d.ts (the editors' declarations) from the same sources: every
# bound name is a member of its namespace's interface, every export a member of `Exports`, every
# permission, setting type and manifest / schema key in its type.
dts_path = ROOT + r'/docs/windock.d.ts'
dts = open(dts_path, encoding='utf-8').read() if os.path.exists(dts_path) else ''
if not dts:
    drift.append('docs/windock.d.ts is missing')
def interface(name):
    m = re.search(r'^\s*interface ' + re.escape(name) + r'\b[^{]*\{', dts, re.M)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(dts) and depth:
        depth += {'{': 1, '}': -1}.get(dts[i], 0)
        i += 1
    return dts[m.end():i - 1]
def member(body, name):
    return re.search(r'^\s*(?:readonly\s+)?' + re.escape(name) + r'\??\s*[(:<]', body, re.M) is not None
dts_names = {'canvas': 'Canvas', 'path': 'Path', 'dock.theme': 'DockTheme', 'dock': 'Dock', 'timer': 'Timer',
             'system': 'System', 'storage': 'Storage', 'shell': 'Shell', 'popup': 'Popup', 'console': 'Console',
             'http': 'Http', 'widget': 'Widget', 'audio': 'Audio', 'media': 'Media', 'notifications': 'Notifications',
             'calendar': 'Calendar', 'weather': 'Weather', 'perf': 'Perf', 'network': 'Network', 'input': 'Input',
             'notify': 'Notify', 'links': 'Links', 'settings': 'Settings'}
api['console'] = api.get('console', set()) | {'log', 'info', 'debug', 'warn', 'error'}
if dts:
    for ns, names in sorted(api.items()):
        if ns == 'global':
            for name in sorted(names):
                if not re.search(r'^declare function ' + re.escape(name) + r'\(', dts, re.M):
                    drift.append(f'windock.d.ts: no "declare function {name}"')
            continue
        body = interface(dts_names.get(ns, '?'))
        if body is None:
            drift.append(f'windock.d.ts: no interface {dts_names.get(ns, "?")} for {ns}')
            continue
        for name in sorted(names & members[ns] if ns in members else names):
            if not member(body, name):
                drift.append(f'windock.d.ts: {ns}.{name} is not in interface {dts_names[ns]}')
        if ns not in ('path', 'dock.theme') and not re.search(r'^declare (?:const|var) ' + re.escape(ns) + r':', dts, re.M):
            drift.append(f'windock.d.ts: no global "declare const {ns}"')
    body = interface('Exports') or ''
    for e in exports:
        if not member(body, e):
            drift.append('windock.d.ts: export ' + e + ' is not in interface Exports')
    ptype = re.search(r'type Permission =(.*?);', dts, re.S)
    for p in sorted(perms):
        if not ptype or "'" + p + "'" not in ptype.group(1):
            if p not in ('http',):  # "http" alone allows nothing: the type has the host forms
                drift.append('windock.d.ts: permission ' + p + ' is not in type Permission')
    types = set(re.findall(r's == L"([a-z]+)"', pkg[pkg.index('SettingField::Type TypeOf('):pkg.index('void ReadSize(')]))
    stype = re.search(r'type SettingType =(.*?);', dts, re.S)
    for t in sorted(types):
        if not stype or "'" + t + "'" not in stype.group(1):
            drift.append('windock.d.ts: setting type ' + t + ' is not in type SettingType')
    if 'kTopKeys' in pkg:
        body = interface('Manifest') or ''
        for k in keys:
            if not member(body, k):
                drift.append('windock.d.ts: manifest key ' + k + ' is not in interface Manifest')
        body = interface('SettingSchema') or ''
        for k in fk:
            if not member(body, k):
                drift.append('windock.d.ts: settings schema key ' + k + ' is not in interface SettingSchema')

print('\n'.join(drift) if drift else 'no drift: docs/widgets.md and docs/windock.d.ts match the bindings, exports, permissions and manifest keys')
print('namespaces checked:', ', '.join(f'{k} ({len(v)})' for k, v in sorted(api.items())) + f'; exports {len(exports)}; permissions {len(perms)}')
sys.exit(1 if drift else 0)
