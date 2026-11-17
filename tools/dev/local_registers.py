# Checks XenonRecomp output built with the *_as_local options (config/*.toml): lists the functions in
# which a local register is live at the entry, that is, read on some path before being written. Such a
# function takes a value from its caller outside the calling convention, which a local register cannot
# carry (patches/README.md, XenonRecomp 0006).
#
# Usage: python tools/dev/local_registers.py build/ppc build/ppc_ai2c
#
# The pass works on the generated C++, statement by statement: labels, gotos, switches and returns give
# the control flow, and the text of each statement gives the registers it reads and writes. It reports
# more than is wrong, never less on the paths it follows:
# - a path is followed even when its conditions exclude each other;
# - a write to any view of a register (u32, f64, one vector element) counts as a write of the register;
# - a non-volatile register stored to memory before being written is taken as the prologue saving it.
import glob
import re
import sys
from collections import defaultdict

FUNCTION = re.compile(r'^PPC_FUNC_IMPL\(__imp__(\w+)\) \{\n(.*?)^\}', re.M | re.S)
DECLARATION = re.compile(r'^\tPPC(?:XER|CR|V)?Register (\w+)\{\};$')
LABEL = re.compile(r'^(loc_\w+):$')
GOTO = re.compile(r'goto (loc_\w+);')
CALL = re.compile(r'^\t(sub_\w+|__\w+)\(ctx, base\);$')
ANY_CALL = re.compile(r'(?<!\w)(sub_\w+|__\w+)\(ctx, base\);')
ASSIGNMENT = re.compile(r'^\t(\w+)(?:\.\w+(?:\[[^\]]*\])?)? = ')
VECTOR_STORE = re.compile(r'^\tsimde_mm_store_(?:si128|ps)\((?:\(simde__m128i\*\))?(v\d+)\.\w+, ')
CR_METHOD = re.compile(r'^\t(cr\d)\.(?:compare|setFromMask)\b')
FIELD_ASSIGNMENT = re.compile(r'^\t(cr\d|xer)\.(lt|gt|eq|so|un|ca) = ')
NAME = re.compile(r'(?<![\w.])([a-z]+\d*)\b')
CR_FIELDS = ('.lt', '.gt', '.eq', '.so', '.un')
# Locals of the generated code that are not guest registers.
SCRATCH = {'temp', 'vTemp', 'ea', 'env', 'reserved'}


def kind(register):
    """The option that makes the register a local: nonarg, nonvolatile, cr, ctr or xer."""
    register = register.split('.')[0]
    if register in ('ctr', 'xer'):
        return register
    if register.startswith('cr'):
        return 'cr'
    number = int(register[1:])
    if register[0] == 'r':
        return 'nonarg' if number in (0, 2, 11, 12) else 'nonvolatile'
    if register[0] == 'f':
        return 'nonarg' if number == 0 else 'nonvolatile'
    return 'nonarg' if 32 <= number <= 63 else 'nonvolatile'


def fold_blocks(lines):
    """Makes one statement of each braced block.

    A switch and its cases (each a goto) become a jump to every case. An `if (...) { ... }` (a
    conditional tail call) becomes a statement that reads what the block reads and falls through.
    """
    folded = []
    position = 0
    while position < len(lines):
        line = lines[position]
        text = line.strip()
        if text.startswith('switch (') or (text.startswith('if (') and text.endswith('{')):
            inner = []
            position += 1
            while position < len(lines) and lines[position].strip() != '}':
                inner.append(lines[position].strip())
                position += 1
            if text.startswith('switch ('):
                line = line.rstrip() + ''.join(f' goto {target};' for target in GOTO.findall(' '.join(inner)))
            else:
                line = line.rstrip() + ' ' + ' '.join(part for part in inner if not part.startswith('//')) + ' }'
        folded.append(line)
        position += 1
    return folded


def live_at_entry(body, never_return):
    """Returns the local registers live at the entry of a function, and the functions it calls."""
    lines = body.split('\n')
    locals_ = {m.group(1) for m in map(DECLARATION.match, lines) if m} - SCRATCH
    calls = ANY_CALL.findall(body)
    if not locals_:
        return set(), calls
    token = re.compile(r'(?<![\w.])(' + '|'.join(sorted(locals_, key=len, reverse=True)) + r')\b(\.\w+)?')

    statements = []  # (registers read, registers written, falls through, jump targets)
    labels = {}
    for line in fold_blocks([line for line in lines if not DECLARATION.match(line)]):
        text = line.strip()
        if not text or text.startswith('//') or text.startswith('PPC_FUNC_PROLOGUE'):
            continue
        label = LABEL.match(text)
        if label:
            labels[label.group(1)] = len(statements)
            continue

        written = set()
        rest = line
        for pattern in (ASSIGNMENT, VECTOR_STORE):
            m = pattern.match(line)
            if m and m.group(1) in locals_ and kind(m.group(1)) not in ('cr', 'xer'):
                written.add(m.group(1))
                rest = line[m.end():]
                break
        m = CR_METHOD.match(line)
        if m and m.group(1) in locals_:
            written |= {m.group(1) + field for field in CR_FIELDS}
            rest = line[m.end():]
        m = FIELD_ASSIGNMENT.match(line)
        if m and m.group(1) in locals_:
            written.add(f'{m.group(1)}.{m.group(2)}')
            rest = line[m.end():]

        # A store to guest memory: PPC_STORE_*, or a vector store whose destination is under `base`.
        destination = rest.split(',')[0]
        store = not written and (text.startswith('PPC_STORE_') or
                                 (text.startswith('simde_mm_store') and '(base + ' in destination))
        mfcr = '? 0x80000000' in line or re.search(r'\|= cr\d\.\w+ \? 0x', line) is not None
        read = set()
        for found in token.finditer(rest):
            register, member = found.group(1), found.group(2) or ''
            if register == 'xer':
                # Every compare copies the summary overflow bit; only the carry carries a value.
                if member == '.ca':
                    read.add('xer.ca')
            elif register.startswith('cr'):
                # mfcr in a prologue saves the caller's condition fields.
                if not mfcr:
                    read |= {register + member} if member in CR_FIELDS else {register + field for field in CR_FIELDS}
            elif store and kind(register) == 'nonvolatile' and register not in NAME.findall(destination):
                pass  # the value of a store: the prologue saving the caller's register
            else:
                read.add(register)

        targets = GOTO.findall(text)
        call = CALL.match(line)
        ends = (text == 'return;' or text.startswith('__builtin_unreachable') or text.startswith('__builtin_trap') or
                (call is not None and call.group(1) in never_return))
        jumps = text.startswith('goto ') or text.startswith('switch (')
        statements.append((read, written, not ends and not jumps, [] if ends else targets))

    count = len(statements)
    successors = []
    for index, (read, written, falls_through, targets) in enumerate(statements):
        following = [labels[target] for target in targets if labels.get(target, count) < count]
        if falls_through and index + 1 < count:
            following.append(index + 1)
        successors.append(following)

    live = [set() for _ in range(count)]
    changed = True
    while changed:
        changed = False
        for index in range(count - 1, -1, -1):
            read, written, _, _ = statements[index]
            after = set().union(*(live[successor] for successor in successors[index])) if successors[index] else set()
            before = read | (after - written)
            if before != live[index]:
                live[index] = before
                changed = True
    return (live[0] if count else set()), calls


def main(directories):
    functions = {}
    for directory in directories:
        for path in sorted(glob.glob(directory + '/ppc_recomp.*.cpp')):
            with open(path, encoding='utf-8', errors='replace') as file:
                for m in FUNCTION.finditer(file.read()):
                    functions[m.group(1)] = m.group(2)

    if not functions:
        sys.exit('no recompiled function found: pass the directories of the generated code (build/ppc build/ppc_ai2c)')

    # A function without any return or jump never returns: the code after a call to it is not reached.
    never_return = {name for name, body in functions.items() if 'return;' not in body and 'goto ' not in body}

    live = {}
    callers = defaultdict(set)
    for name, body in functions.items():
        registers, calls = live_at_entry(body, never_return)
        if registers:
            live[name] = registers
        for callee in calls:
            callers[callee].add(name)

    called = sorted(name for name in live if callers.get(name))
    print(f'{len(functions)} functions, {len(never_return)} never return')
    print(f'{len(live)} have a local register live at the entry; {len(called)} of them are called directly:')
    for name in called:
        print(f'  {name}  live: {" ".join(sorted(live[name]))}  called by: {" ".join(sorted(callers[name]))}')
    print('The others are reached only through a pointer or not at all (exception funclets, helpers).')


if __name__ == '__main__':
    main(sys.argv[1:])
