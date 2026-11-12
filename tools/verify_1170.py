"""Audit AMR's 1.6.1170 callsites and vtables in an unpacked SkyrimSE.exe.

Requires Python 3 and capstone (pip install capstone).
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def mappings(path):
    data = path.read_bytes()
    at = 0

    def read(fmt):
        nonlocal at
        values = struct.unpack_from('<' + fmt, data, at)
        at += struct.calcsize('<' + fmt)
        return values[0] if len(values) == 1 else values

    if read('I') not in (1, 2):
        raise ValueError('Unsupported Address Library format')
    version = read('4I')
    name_size = read('I')
    at += name_size
    pointer_size, count = read('2I')
    result = {}
    previous_id = previous_offset = 0

    def decode(code, previous):
        if code == 0: return read('Q')
        if code == 1: return previous + 1
        if code == 2: return previous + read('B')
        if code == 3: return previous - read('B')
        if code == 4: return previous + read('H')
        if code == 5: return previous - read('H')
        if code == 6: return read('H')
        if code == 7: return read('I')
        raise ValueError('Invalid Address Library entry')

    for _ in range(count):
        kind = read('B')
        entry_id = decode(kind & 15, previous_id)
        high = kind >> 4
        offset = decode(high & 7, previous_offset // pointer_size if high & 8 else previous_offset)
        if high & 8:
            offset *= pointer_size
        result[entry_id] = offset
        previous_id, previous_offset = entry_id, offset
    if at != len(data) or version != (1, 6, 1170, 0) or pointer_size != 8:
        raise ValueError('Expected the complete x64 Address Library for 1.6.1170.0')
    return result


def verify(executable, library):
    addresses = mappings(library)
    data = executable.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Expected a PE executable')
    section_count, optional_size = struct.unpack_from('<H12xH', data, pe + 6)
    base = struct.unpack_from('<Q', data, pe + 48)[0]
    sections = []
    for index in range(section_count):
        at = pe + 24 + optional_size + index * 40
        _, rva, size, raw = struct.unpack_from('<4I', data, at + 8)
        sections.append((rva, size, raw))

    def read(rva, length):
        for start, size, raw in sections:
            if start <= rva and rva + length <= start + size:
                offset = raw + rva - start
                return data[offset:offset + length]
        raise ValueError(f'RVA {rva:#x} is not backed by file data')

    decoder = Cs(CS_ARCH_X86, CS_MODE_64)

    def disasm(rva, length):
        return [f'{ins.address:#x}: {ins.mnemonic} {ins.op_str}'
                for ins in decoder.disasm(read(rva, length), base + rva)]

    callsites = []
    for name, entry_id, delta, expected in [
        ('Activate/ComputeStartTime', 59252, 0x66E, 0xAD1050),
        ('Deactivate/ResetIgnoreStartTime', 59254, 0x1A, 0xAD11E0),
        ('Translation1', 32703, 0x298, addresses[32578]),
        ('Translation2', 32703, 0x2AA, addresses[32578]),
        ('Rotation1', 32703, 0x35C, addresses[32579]),
        ('Rotation2', 32703, 0x36D, addresses[32579]),
        ('UpdateStateAndMovement/GetState', 78275, 0x7C2, 0xB7AE60),
        ('GraphCreate1', 20108, 0x64, addresses[63555]),
        ('GraphCreate2', 32973, 0x5A, addresses[63555]),
        ('GraphCreate3', 32989, 0x5A, addresses[63555]),
        ('GraphCreate4', 37979, 0x7A, addresses[63555]),
        ('GraphCreate5', 40753, 0x64, addresses[63555]),
        ('GraphCreate6', 63547, 0x1F, addresses[63555]),
        ('GraphDestroy1', 63547, 0x38, addresses[63556]),
        ('GraphDestroy2', 63758, 0x19, addresses[63556]),
    ]:
        rva = addresses[entry_id] + delta
        code = read(rva, 5)
        target = rva + 5 + struct.unpack_from('<i', code, 1)[0]
        callsites.append(dict(name=name, id=entry_id, function_rva=hex(addresses[entry_id]),
                              delta=hex(delta), rva=hex(rva), bytes=code.hex(),
                              target_rva=hex(target), valid=code[0] == 0xE8 and target == expected))

    tail_rva = 0x166E737
    tail = read(tail_rva, 5)
    tail_target = tail_rva + 5 + struct.unpack_from('<i', tail, 1)[0]
    callsites.append(dict(name='GraphDestroyTail', rva=hex(tail_rva), bytes=tail.hex(),
                          target_rva=hex(tail_target), valid=tail[0] == 0xE9 and tail_target == addresses[63556]))
    references = {addresses[63555]: set(), addresses[63556]: set()}
    for start, size, raw in sections:
        section = data[raw:raw + size]
        for opcode in (0xE8, 0xE9):
            at = section.find(bytes([opcode]))
            while at >= 0:
                if at + 5 <= len(section):
                    target = start + at + 5 + struct.unpack_from('<i', section, at + 1)[0]
                    if target in references:
                        references[target].add(start + at)
                at = section.find(bytes([opcode]), at + 1)
    expected_create = {int(item['rva'], 16) for item in callsites if item['name'].startswith('GraphCreate')}
    expected_destroy = {int(item['rva'], 16) for item in callsites if item['name'].startswith('GraphDestroy')}
    coverage = dict(constructor_references=sorted(map(hex, references[addresses[63555]])),
                    destructor_references=sorted(map(hex, references[addresses[63556]])),
                    valid=references[addresses[63555]] == expected_create and
                          references[addresses[63556]] == expected_destroy)

    vtables = []
    for name, entry_id, slot, expected in [
        ('bhkCharacterStateOnGround', 240833, 8, 0xF02550),
        ('bhkCharProxyController/controller/GetPosition', 240560, 2, 0xED1DC0),
        ('bhkCharProxyController/controller/GetCollisionFilterInfo', 240560, 8, 0xED2020),
        ('bhkCharProxyController/controller/GetHavokWorld', 240560, 15, 0xED1C60),
        ('bhkCharRigidBodyController/GetPosition', 240580, 2, 0xED42F0),
        ('bhkCharRigidBodyController/GetCollisionFilterInfo', 240580, 8, 0xED4710),
        ('bhkCharRigidBodyController/GetHavokWorld', 240580, 15, 0xED3F70),
        ('bhkWorld/PickObject', 238918, 0x33, 0xE86560),
        ('INISettingCollection/ReadAllSettings', 187074, 9, 0x18CED0),
        ('BShkbAnimationGraph/DeleteThis', 232870, 0, 0xBBCF30),
    ]:
        target = struct.unpack('<Q', read(addresses[entry_id] + slot * 8, 8))[0] - base
        vtables.append(dict(name=name, id=entry_id, table_rva=hex(addresses[entry_id]),
                            slot=hex(slot), target_rva=hex(target), valid=target == expected))

    evidence = {
        'activate_context_r15': disasm(addresses[59252], 0x80),
        'deactivate_context_rsi': disasm(addresses[59254], 0x30),
        'motion_character_r13': disasm(addresses[32703], 0x90),
        'motion_clip_name_rbx_minus_14': disasm(addresses[32703] + 0x1B0, 0x40),
        'translation_wrapper': disasm(addresses[32578], 0x41),
        'rotation_wrapper': disasm(addresses[32579], 0x28),
        'state_query_and_z_velocity': disasm(addresses[78275] + 0x7BF, 0x38),
        'gravity_pointer_return': disasm(addresses[77838], 0xA8),
        'movement_input': disasm(addresses[81002], 0xC0),
        'rotation_interpolation': disasm(addresses[70836], 0x60),
        'graph_constructor': disasm(addresses[63555], 0x23C),
        'graph_destructor': disasm(addresses[63556], 0x3A0),
        'graph_delete_wrapper': disasm(addresses[63758], 0x50),
        'graph_eh_cleanup_tail': disasm(tail_rva - 0x10, 0x15),
    }
    helpers = [dict(id=entry_id, rva=hex(addresses[entry_id]), valid=addresses[entry_id] == expected)
               for entry_id, expected in [(32582, 0x538390), (32583, 0x538630),
                                          (70836, 0xD340E0), (77838, 0xE84230), (81002, 0xF3D670),
                                          (63555, 0xBAD070), (63556, 0xBAD2C0), (63758, 0xBBCF30)]]
    return dict(runtime='1.6.1170.0', image_base=hex(base),
                executable_sha256=hashlib.sha256(data).hexdigest(),
                library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
                callsites=callsites, vtables=vtables, helpers=helpers, evidence=evidence, lifecycle_coverage=coverage,
                valid=coverage['valid'] and all(item['valid'] for item in callsites + vtables + helpers))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('address_library', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = verify(args.executable, args.address_library)
    encoded = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(encoded, encoding='utf-8')
    print(f"AMR 1.6.1170 audit: {'PASS' if result['valid'] else 'FAIL'}; "
          f"{len(result['callsites'])} callsites, {len(result['vtables'])} virtual entries, "
          f"{len(result['helpers'])} helper RVAs")
    raise SystemExit(0 if result['valid'] else 1)
