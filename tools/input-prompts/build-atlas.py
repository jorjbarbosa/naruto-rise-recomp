"""Builds the button glyph atlas from Kenney's Input Prompts (CC0).

usage: python -I tools/input-prompts/build-atlas.py <extracted Kenney "Input Prompts" folder>

Writes, from the 128 px ("Double") images:
  app/ui/images/prompts.png    the atlas, one 128 px cell per glyph
  app/ui/prompts.rcss          an RmlUi @spritesheet naming each cell
  app/src/controls/glyph_atlas.inc   the same names and cells for C++

Only the standard library: the Kenney PNGs are indexed or RGBA, 8 bits or less.
"""
import struct
import sys
import zlib
from pathlib import Path

CELL = 128
COLUMNS = 16
ROOT = Path(__file__).resolve().parents[2]

PAD_INPUTS = ['south', 'east', 'west', 'north', 'lb', 'rb', 'lt', 'rt', 'ls', 'rs', 'back', 'start']
DIRECTIONS = ['', '-up', '-down', '-left', '-right', '-horizontal', '-vertical']


def pad_family(prefix, folder, files, dpad, lstick, rstick):
    """files: the 12 PAD_INPUTS; dpad/lstick/rstick: Kenney stems without the direction suffix."""
    out = [(f'{prefix}-{name}', folder, stem) for name, stem in zip(PAD_INPUTS, files)]
    for name, stem in (('dpad', dpad), ('lstick', lstick), ('rstick', rstick)):
        out += [(f'{prefix}-{name}{d}', folder, stem + d.replace('-', '_')) for d in DIRECTIONS]
    return out


GLYPHS = []
# Faces in colour, everything else in the family's single colour. Positional
# names: south is A on Xbox, cross on PlayStation and B on Nintendo.
GLYPHS += pad_family('xbox', 'Xbox Series', [
    'xbox_button_color_a', 'xbox_button_color_b', 'xbox_button_color_x', 'xbox_button_color_y',
    'xbox_lb', 'xbox_rb', 'xbox_lt', 'xbox_rt', 'xbox_ls', 'xbox_rs', 'xbox_button_back', 'xbox_button_start'],
    'xbox_dpad', 'xbox_stick_l', 'xbox_stick_r')
GLYPHS += pad_family('ps', 'PlayStation Series', [
    'playstation_button_color_cross', 'playstation_button_color_circle', 'playstation_button_color_square',
    'playstation_button_color_triangle', 'playstation_trigger_l1', 'playstation_trigger_r1',
    'playstation_trigger_l2', 'playstation_trigger_r2', 'playstation_button_l3', 'playstation_button_r3',
    'playstation5_button_create', 'playstation5_button_options'],
    'playstation_dpad', 'playstation_stick_l', 'playstation_stick_r')
GLYPHS += pad_family('switch', 'Nintendo Switch', [
    'switch_button_b', 'switch_button_a', 'switch_button_y', 'switch_button_x',
    'switch_button_l', 'switch_button_r', 'switch_button_zl', 'switch_button_zr',
    'switch_stick_l_press', 'switch_stick_r_press', 'switch_button_minus', 'switch_button_plus'],
    'switch_dpad', 'switch_stick_l', 'switch_stick_r')
GLYPHS += pad_family('deck', 'Steam Deck', [
    'steamdeck_button_a', 'steamdeck_button_b', 'steamdeck_button_x', 'steamdeck_button_y',
    'steamdeck_button_l1', 'steamdeck_button_r1', 'steamdeck_button_l2', 'steamdeck_button_r2',
    'steamdeck_stick_l_press', 'steamdeck_stick_r_press', 'steamdeck_button_view', 'steamdeck_button_options'],
    'steamdeck_dpad', 'steamdeck_stick_l', 'steamdeck_stick_r')

# Keys by the SDK's key names (app/src/menu/key_names.cpp). A key without a
# glyph here (F13-F24) is drawn as text by its consumer.
KEYS = {chr(c): f'keyboard_{chr(c).lower()}' for c in range(ord('A'), ord('Z') + 1)}
KEYS.update({str(d): f'keyboard_{d}' for d in range(10)})
KEYS.update({f'F{n}': f'keyboard_f{n}' for n in range(1, 13)})
KEYS.update({f'Numpad{d}': f'keyboard_{d}' for d in range(10)})
KEYS.update({
    'NumpadStar': 'keyboard_asterisk', 'NumpadPlus': 'keyboard_numpad_plus', 'NumpadMinus': 'keyboard_minus',
    'NumpadSlash': 'keyboard_slash_forward', 'Return': 'keyboard_enter', 'Escape': 'keyboard_escape',
    'Backspace': 'keyboard_backspace', 'Tab': 'keyboard_tab', 'Space': 'keyboard_space',
    'Left': 'keyboard_arrow_left', 'Up': 'keyboard_arrow_up', 'Right': 'keyboard_arrow_right',
    'Down': 'keyboard_arrow_down', 'PageUp': 'keyboard_page_up', 'PageDown': 'keyboard_page_down',
    'Home': 'keyboard_home', 'End': 'keyboard_end', 'Insert': 'keyboard_insert', 'Delete': 'keyboard_delete',
    'Shift': 'keyboard_shift', 'Control': 'keyboard_ctrl', 'Alt': 'keyboard_alt',
    'CapsLock': 'keyboard_capslock', 'Pause': 'keyboard_pause', 'PrintScreen': 'keyboard_printscreen',
    'ScrollLock': 'keyboard_scroll_lock', 'NumLock': 'keyboard_numlock', 'Semicolon': 'keyboard_semicolon',
    'Plus': 'keyboard_equals', 'Comma': 'keyboard_comma', 'Minus': 'keyboard_minus',
    'Period': 'keyboard_period', 'Slash': 'keyboard_slash_forward', 'Backtick': 'keyboard_tilde',
    'LBracket': 'keyboard_bracket_open', 'Backslash': 'keyboard_slash_back',
    'RBracket': 'keyboard_bracket_close', 'Quote': 'keyboard_apostrophe',
    # Mouse buttons and motion, and the key groups the menus name.
    'LMB': 'mouse_left', 'RMB': 'mouse_right', 'MMB': 'mouse_scroll', 'Mouse': 'mouse_move',
    'MouseVertical': 'mouse_vertical', 'MouseHorizontal': 'mouse_horizontal',
    'Arrows': 'keyboard_arrows', 'ArrowsHorizontal': 'keyboard_arrows_horizontal',
    'ArrowsVertical': 'keyboard_arrows_vertical',
})
GLYPHS += [(f'key-{name.lower()}', 'Keyboard & Mouse', stem) for name, stem in KEYS.items()]


def read_png(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    at, idat, palette, trns = 8, b'', None, None
    while at < len(data):
        length, kind = struct.unpack_from('>I4s', data, at)
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b'IHDR':
            width, height, depth, color, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
    assert interlace == 0 and depth <= 8, path
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    stride = (width * channels * depth + 7) // 8
    bpp = max(1, channels * depth // 8)
    raw = zlib.decompress(idat)
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = previous[i]
            c = previous[i - bpp] if i >= bpp else 0
            if kind == 1: line[i] = (line[i] + a) & 255
            elif kind == 2: line[i] = (line[i] + b) & 255
            elif kind == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        previous = line
    rgba = bytearray(width * height * 4)
    for y, line in enumerate(rows):
        for x in range(width):
            if depth == 8:
                s = line[x * channels:(x + 1) * channels]
            else:
                bit = x * depth
                s = [(line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)]
            if color == 3:
                r, g, b = palette[s[0]]
                a = trns[s[0]] if trns and s[0] < len(trns) else 255
            elif color == 6: r, g, b, a = s
            elif color == 2: r, g, b, a = *s, 255
            elif color == 4: r = g = b = s[0]; a = s[1]
            else: r = g = b = s[0] * 255 // ((1 << depth) - 1); a = 255
            rgba[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes((r, g, b, a))
    return width, height, rgba


FILL = (30, 30, 34)


def fill_holes(width, height, rgba):
    """Kenney's legends are cut out of the glyph. Fill what the outline encloses
    in a dark tone, so a legend stays legible over any background."""
    outside = bytearray(width * height)
    stack = [i for i in range(width * height)
             if (i % width in (0, width - 1) or i // width in (0, height - 1)) and rgba[i * 4 + 3] < 255]
    for i in stack:
        outside[i] = 1
    while stack:
        i = stack.pop()
        x, y = i % width, i // width
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            n = ny * width + nx
            if 0 <= nx < width and 0 <= ny < height and not outside[n] and rgba[n * 4 + 3] < 255:
                outside[n] = 1
                stack.append(n)
    for i in range(width * height):
        a = rgba[i * 4 + 3]
        if outside[i] or a == 255:
            continue
        for c in range(3):
            rgba[i * 4 + c] = (rgba[i * 4 + c] * a + FILL[c] * (255 - a)) // 255
        rgba[i * 4 + 3] = 255


def write_png(path, width, height, rgba):
    raw = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    kenney = Path(sys.argv[1])
    rows = (len(GLYPHS) + COLUMNS - 1) // COLUMNS
    width, height = COLUMNS * CELL, rows * CELL
    atlas = bytearray(width * height * 4)
    sprites, cells = [], []
    for index, (name, folder, stem) in enumerate(GLYPHS):
        w, h, rgba = read_png(kenney / folder / 'Double' / f'{stem}.png')
        assert (w, h) == (CELL, CELL), (stem, w, h)
        fill_holes(w, h, rgba)
        x, y = index % COLUMNS * CELL, index // COLUMNS * CELL
        for row in range(CELL):
            o = ((y + row) * width + x) * 4
            atlas[o:o + CELL * 4] = rgba[row * CELL * 4:(row + 1) * CELL * 4]
        sprites.append(f'  {name}: {x}px {y}px {CELL}px {CELL}px;')
        cells.append(f'{{"{name}", {x}, {y}}},')
    write_png(ROOT / 'app/ui/images/prompts.png', width, height, bytes(atlas))
    header = 'Generated by tools/input-prompts/build-atlas.py from Kenney Input Prompts (CC0, www.kenney.nl).'
    (ROOT / 'app/ui/prompts.rcss').write_text(
        f'/* {header} */\n@spritesheet prompts {{\n  src: images/prompts.png;\n  resolution: 1x;\n' +
        '\n'.join(sprites) + '\n}\n', encoding='utf8', newline='\n')
    (ROOT / 'app/src/controls/glyph_atlas.inc').write_text(
        f'// {header}\n// {{name, x, y}} of each {CELL} px cell of app/ui/images/prompts.png.\n' +
        '\n'.join(cells) + '\n', encoding='utf8', newline='\n')
    print(len(GLYPHS), 'glyphs,', f'{width}x{height}')


main()
