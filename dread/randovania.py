# SwitchRecomp Randovania compatibility generation; GPL-3.0.
# The replacement instructions implement open-dread-rando's patches:
# https://github.com/randovania/open-dread-rando/blob/main/tools/create_exefs_patches.py
# https://github.com/randovania/open-dread-rando/blob/main/tools/exefs_sources/debug_input.s
"""Add opt-in Randovania functions after the verified, pristine Dread patch pass.

No original game instructions or generated game code are stored here. The input
Program and generated files come from the player's verified 2.1.0 ExeFS. Runtime
validation of an enabled mod controls the exported flag before guest execution.
"""
import difflib
import re
import struct
from pathlib import Path

import a64
import codegen


MAIN_ID = bytes.fromhex('646761f643afebb379edd5e6a5151af2cef93dc1')
MAIN_BASE = 0x8005000
FLAG = 'swr_randovania_enabled'
MARKER = '/* Randovania compatibility, revision 1 */'

# Upstream-authored replacements, offsets relative to main (not the NSO header).
# The two BL encodings and lua_pushinteger call in DEBUG_WORDS are PC-relative.
DEBUG_OFFSET = 0x10943A0
DEBUG_WORDS = (
    0xA9B87BFD, 0x910003FD, 0xF9000BF3, 0xAA0003F3, 0x52800400,
    0x9100A3E1, 0xB9002BFF, 0xB9002FE0, 0x9100C3E0, 0x94069403,
    0x9100B3E1, 0x910163E0, 0x94069404, 0xF9401FE1, 0xF94033E0,
    0xAA000021, 0xAA1303E0, 0x94003883, 0xD2800020, 0xF9400BF3,
    0xA8C87BFD, 0xD65F03C0,
)
CODE_PATCHES = {
    0xDA7380: bytes.fromhex('81410090 21B82091'),  # Corpius grants ITEM_NONE
    0x11E7F1C: bytes.fromhex('1F2003D5'),          # omit Aeion unlock message
    0xAE9D70: bytes.fromhex('027D8052'),           # door lock capacity: 1000
    DEBUG_OFFSET: struct.pack('<22I', *DEBUG_WORDS),
}


class PatchConflict(ValueError):
    """A verified base patch and the randomizer need the same generated lines."""


def merge_changes(original, current, replacement):
    """Merge a generated instruction change without losing existing base edits.

    Every replaced original line must survive unchanged in current. Insertions
    need an unambiguous boundary. Overlapping edits fail instead of choosing one.
    """
    old, have, new = (s.splitlines(keepends=True) for s in (original, current, replacement))
    base_edits = difflib.SequenceMatcher(None, old, have, autojunk=False).get_opcodes()
    edits = []
    for tag, first, end, nfirst, nend in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if tag == 'equal':
            continue
        start_at = finish_at = None
        if first < end:
            for btag, bfirst, bend, cfirst, cend in base_edits:
                if btag == 'equal' and bfirst <= first and end <= bend:
                    start_at = cfirst + first - bfirst
                    finish_at = start_at + end - first
                    break
        else:
            # Conservatively reject a second edit at the same insertion point.
            conflict = any(btag != 'equal' and bfirst <= first <= bend
                           for btag, bfirst, bend, _, _ in base_edits)
            if not conflict:
                for btag, bfirst, bend, cfirst, cend in base_edits:
                    if btag == 'equal' and bfirst <= first <= bend:
                        start_at = finish_at = cfirst + first - bfirst
                        break
        if start_at is None:
            raise PatchConflict('Randovania overlaps a base game patch; review the generated function before rebuilding')
        edits.append((start_at, finish_at, new[nfirst:nend]))
    for first, end, lines in reversed(edits):
        have[first:end] = lines
    return ''.join(have)


class _PatchedProgram:
    def __init__(self, prog, words):
        self.prog, self.words = prog, words

    def decode(self, address):
        if address in self.words:
            word = self.words[address]
            return word, a64.emit_static(word, address)
        return self.prog.decode(address)


def _variant_function(prog, function, words):
    """Retain existing entry points and add the replacement's control-flow edges."""
    patched = _PatchedProgram(prog, words)
    variant = codegen.Func(function.start, function.addrs, function.mod)
    variant.name = function.name
    variant.leaders = set(function.leaders)
    previous_end = False
    for address in variant.addrs:
        if previous_end:
            variant.leaders.add(address)
        _, decoded = patched.decode(address)
        previous_end = False
        if isinstance(decoded, tuple):
            kind = decoded[0]
            if kind in ('b', 'bc') and decoded[-1] in variant.aset:
                variant.leaders.add(decoded[-1])
            if kind in ('bl', 'blr') and address + 4 in variant.aset:
                variant.leaders.add(address + 4)
            previous_end = kind in codegen.UNCOND
        elif decoded is None:
            previous_end = True
    return codegen.emit_function(patched, variant, prog.funcs)


def wrap_function(name, current, randomized):
    """The original symbol remains the entry for direct calls and dispatch alike."""
    declaration = 'void %s(CPU* RESTRICT c, u64 e){' % name
    if not current.startswith(declaration) or not randomized.startswith(declaration):
        raise PatchConflict('Unexpected generated declaration for ' + name)
    normal = current.replace(declaration, 'static void %s_vanilla(CPU* RESTRICT c, u64 e){' % name, 1)
    changed = randomized.replace(declaration, 'static void %s_randomized(CPU* RESTRICT c, u64 e){' % name, 1)
    wrapper = ('%s\n if(%s) %s_randomized(c,e);\n else %s_vanilla(c,e);\n}'
               % (declaration, FLAG, name, name))
    return normal + '\n' + changed + '\n' + wrapper


def apply(prog, build, files):
    """Patch fresh generated output after dread_setup.apply_patches; fail closed.

    Nothing is written until all expected functions and merge conflicts have
    been checked. Reapplying is rejected: setup's generation key handles reuse.
    """
    main = next((m for m in prog.mods if m.name == 'main'), None)
    if main is None or main.base != MAIN_BASE or main.build_id.rstrip(b'\0') != MAIN_ID:
        raise ValueError('Randovania compatibility requires the verified Metroid Dread 2.1.0 main')
    words = {}
    for offset, data in CODE_PATCHES.items():
        for index, (word,) in enumerate(struct.iter_unpack('<I', data)):
            address = main.base + offset + index * 4
            if prog.word(address) is None:
                raise ValueError('Randovania instruction is outside main text')
            words[address] = word
    affected = {}
    for address in words:
        owners = [f for f in prog.funcs.values() if address in f.aset]
        if len(owners) != 1:
            raise ValueError('Expected one generated function for Randovania instruction %X' % address)
        affected[owners[0].start] = owners[0]

    paths = [Path(p) for p in files]
    module = Path(build) / 'module.c'
    sources = {}
    endings = {}
    for path in paths:
        raw = path.read_bytes().decode('latin-1')
        if MARKER in raw or re.search(r'\b' + FLAG + r'\b', raw):
            raise ValueError('Randovania compatibility already generated; regenerate before applying it again')
        sources[path] = raw.replace('\r\n', '\n')
        endings[path] = '\r\n' if '\r\n' in raw else '\n'
    if module not in sources:
        raise ValueError('Generated module.c is missing from the compilation list')

    changed = set()
    for function in affected.values():
        pattern = re.compile(r'^void ' + re.escape(function.name) + r'\(CPU\* RESTRICT c, u64 e\)\{\n.*?^\}', re.M | re.S)
        matches = [(path, match) for path, source in sources.items() for match in pattern.finditer(source)]
        if len(matches) != 1:
            raise ValueError('Expected one generated body for ' + function.name)
        path, match = matches[0]
        original = codegen.emit_function(prog, function, prog.funcs)
        variant = _variant_function(prog, function, words)
        current = match.group()
        randomized = merge_changes(original, current, variant)
        wrapped = wrap_function(function.name, current, randomized)
        sources[path] = sources[path][:match.start()] + wrapped + sources[path][match.end():]
        changed.add(path)

    for path in changed:
        # New debug-input calls may reference functions not previously used by this chunk.
        refs = sorted(set(re.findall(r'\bF_[0-9A-F]+\b', sources[path])))
        prototypes = ''.join('void %s(CPU* RESTRICT c, u64 e);\n' % name for name in refs)
        sources[path] = sources[path].replace('#include "cpu.h"\n', '#include "cpu.h"\n' + MARKER + '\nextern unsigned ' + FLAG + ';\n' + prototypes, 1)
    sources[module] += '\n' + MARKER + '\nSWR_EXPORT unsigned ' + FLAG + ' = 0;\n'
    changed.add(module)
    for path in changed:
        path.write_bytes(sources[path].replace('\n', endings[path]).encode('latin-1'))
    return len(affected)
