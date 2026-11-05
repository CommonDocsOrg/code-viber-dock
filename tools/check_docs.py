# Checks the user guides (docs/*.md) and README.md's "Documentation" section: every Markdown link
# to a file resolves (relative to the linking file) and every #anchor names a heading of its target
# (GitHub's slugs: lower case, punctuation other than '-' / '_' dropped, spaces to '-', "-1" ...
# for repeats). Also every help topic the settings windows open (the "guide-x.md#anchor" literals
# in src/*.cpp / *.h: F1, the cards' "?" buttons, the Help row) resolves to a guide and a heading,
# and the guides the Help window shows use only the Markdown its converter knows (no tables,
# images, HTML or block quotes). Prints the problems; exit code 1 when there is any.
#   python tools/check_docs.py [repo root]
import glob, os, re, sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
DOCS = os.path.join(ROOT, 'docs')
GUIDES = ['quick-start.md', 'guide-settings.md', 'guide-docks.md', 'guide-items.md', 'troubleshooting.md', 'README.md']

def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()

def strip_code(text):
    # Fenced blocks are not headings or links.
    out, fenced = [], False
    for line in text.split('\n'):
        if line.lstrip().startswith('```'):
            fenced = not fenced
            out.append('')
            continue
        out.append('' if fenced else line)
    return out

def plain(heading):
    h = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', heading)  # links: their text
    h = h.replace('`', '').replace('**', '').replace('*', '')
    return h.strip()

def slug(heading):
    s = re.sub(r'[^\w\- ]', '', plain(heading).lower())
    return s.replace(' ', '-')

anchors_cache = {}
def anchors(path):
    path = os.path.normcase(os.path.abspath(path))
    if path not in anchors_cache:
        seen, found = {}, set()
        for line in strip_code(read(path)):
            m = re.match(r'^(#{1,6})\s+(.*?)\s*#*\s*$', line)
            if not m:
                continue
            base = slug(m.group(2))
            n = seen.get(base, 0)
            found.add(base if n == 0 else f'{base}-{n}')
            seen[base] = n + 1
        anchors_cache[path] = found
    return anchors_cache[path]

problems = []

def check_links(path, lines, label):
    here = os.path.dirname(path)
    for no, line in enumerate(lines, 1):
        for text, target in re.findall(r'\[([^\]]*)\]\(([^)\s]*)\)', re.sub(r'`[^`]*`', '', line)):
            if re.match(r'^[a-z]+:', target):
                if not re.match(r'^(https?|mailto):', target):
                    problems.append(f'{label}:{no}: unsupported link "{target}"')
                continue
            file, _, anchor = target.partition('#')
            dest = os.path.normpath(os.path.join(here, file)) if file else path
            if not os.path.exists(dest):
                problems.append(f'{label}:{no}: "{target}": no file {os.path.relpath(dest, ROOT)}')
                continue
            if anchor and dest.endswith('.md') and anchor not in anchors(dest):
                problems.append(f'{label}:{no}: "{target}": no heading #{anchor} in {os.path.relpath(dest, ROOT)}')

# 1. The guides (and the rest of docs/).
for path in sorted(glob.glob(os.path.join(DOCS, '*.md'))):
    name = os.path.basename(path)
    lines = strip_code(read(path))
    check_links(path, lines, 'docs/' + name)
    if name in GUIDES:
        for no, line in enumerate(lines, 1):
            if re.match(r'^\s*\|.*\|\s*$', line):
                problems.append(f'docs/{name}:{no}: a table (the Help window shows no tables)')
            if re.search(r'!\[[^\]]*\]\(', line):
                problems.append(f'docs/{name}:{no}: an image (the Help window shows no images)')
            if re.match(r'^\s*>', line):
                problems.append(f'docs/{name}:{no}: a block quote (the Help window shows none)')
            if re.search(r'<[a-zA-Z/][^>]*>', re.sub(r'`[^`]*`', '', line)):
                problems.append(f'docs/{name}:{no}: HTML (the Help window shows none)')
for name in GUIDES:
    if not os.path.exists(os.path.join(DOCS, name)):
        problems.append(f'docs/{name}: missing (res/app.rc embeds it)')

# 2. README.md's "Documentation" section.
readme = os.path.join(ROOT, 'README.md')
rl = strip_code(read(readme))
start = next((i for i, l in enumerate(rl) if re.match(r'^##\s+Documentation\s*$', l)), None)
if start is None:
    problems.append('README.md: no "## Documentation" section')
else:
    end = next((i for i in range(start + 1, len(rl)) if re.match(r'^##\s', rl[i])), len(rl))
    section = [''] * start + rl[start:end]
    check_links(readme, section, 'README.md')

# 3. The help topics in the sources.
topics = set()
for path in glob.glob(os.path.join(ROOT, 'src', '*.cpp')) + glob.glob(os.path.join(ROOT, 'src', '*.h')):
    for t in re.findall(r'L"((?:quick-start|guide-[a-z]+|troubleshooting|README)\.md(?:#[^"]*)?)"', read(path)):
        topics.add((t, os.path.basename(path)))
for t, src in sorted(topics):
    file, _, anchor = t.partition('#')
    dest = os.path.join(DOCS, file)
    if file not in GUIDES or not os.path.exists(dest):
        problems.append(f'src/{src}: topic "{t}": not a guide')
    elif anchor and anchor not in anchors(dest):
        problems.append(f'src/{src}: topic "{t}": no heading #{anchor} in docs/{file}')

for p in problems:
    print(p)
print(f'{len(topics)} help topics, {len(problems)} problems' if problems else f'Docs clean ({len(topics)} help topics checked).')
sys.exit(1 if problems else 0)
