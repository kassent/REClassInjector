"""Read-only TDB81 differential audit; no hooks, allocations or process writes.

Reuse the ObjectInfo skill's ProcessReader. Pin the PID/TDB/build to the current
host startup log, and compare normalized shapes rather than numerical type IDs.
"""
from __future__ import annotations
import argparse
import collections
import functools
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import struct as S
import tomllib


MASK19 = (1 << 19) - 1


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument('--log', type=Path, default=Path('D:/MO2/mhwilds/overwrite/logs/ClassInjector.log'))
    ap.add_argument('--baseline', type=Path, default=Path('build/CISnapshot.bin'))
    ap.add_argument('--output', type=Path, default=Path('build/ClassInjectorTypeAudit.json'))
    ap.add_argument('--reader', type=Path, default=Path.home() / '.codex/skills/reengine-objectinfo-analysis/scripts/dump_objectinfo.py')
    args = ap.parse_args()
    reader_spec = importlib.util.spec_from_file_location('objectinfo_reader', args.reader)
    module = importlib.util.module_from_spec(reader_spec)
    reader_spec.loader.exec_module(module)
    log = args.log.read_text(encoding='utf-8-sig', errors='replace')
    pid = int(re.search(r'Bootstrap loaded: pid=(\d+)', log)[1])
    tdb_address = int(re.search(r'Metadata relocated: success=1, TDB=([\dA-F]+)', log)[1], 16)
    original_count, final_count = map(int, re.search(r'Published metadata: (\d+) -> (\d+) types', log).groups())
    method_base = int(re.search(r'method base=([\dA-F]+)', log)[1], 16)
    baseline_header = args.baseline.open('rb').read(240)
    original_methods = S.unpack_from('<I', baseline_header, 20)[0]
    old_param_count = S.unpack_from('<I', baseline_header, 52)[0]
    original_parameter_mask = (1 << (old_param_count - 1).bit_length()) - 1
    declarations = {}
    for manifest in re.findall(r'Accepted plugin: (.*?\.toml),', log):
        for decl in tomllib.loads(Path(manifest).read_text(encoding='utf-8-sig')).get('types', []):
            declarations[decl['name']] = {**decl, 'manifest': str(manifest)}

    with module.ProcessReader(pid) as process:
        header = process.read(tdb_address, 240)
        if header[:8] != b'TDB\0Q\0\0\0' or S.unpack_from('<I', header, 8)[0] != final_count:
            raise ValueError('startup log does not describe the current relocated TDB81')
        if S.unpack_from('<I', baseline_header, 8)[0] != original_count:
            raise ValueError('baseline snapshot has a different original type boundary')
        tables = {}
        for name, ptr, count, stride in [('types', 104, 8, 80), ('impls', 112, 28, 48), ('methods', 120, 20, 12),
                                         ('mimpls', 128, 36, 12), ('fields', 136, 24, 8), ('fimpls', 144, 32, 12),
                                         ('params', 176, 52, 12), ('strings', 216, 84, 1), ('bytes', 224, 88, 1)]:
            tables[name] = process.read(S.unpack_from('<Q', header, ptr)[0], S.unpack_from('<I', header, count)[0] * stride)
        rows = list(S.iter_unpack('<QQIIIIQIIQQQQ', tables['types']))
        impls = list(S.iter_unpack('<iiiiQHhh18s', tables['impls']))

        @functools.lru_cache(None)
        def text(offset):
            return tables['strings'][offset:tables['strings'].index(0, offset)].decode('utf-8', errors='replace')

        @functools.lru_cache(None)
        def typ(index):
            row = rows[index]
            impl_index = (row[1] >> 38) & 0x3ffff
            imp = impls[impl_index]
            packed = row[9]
            iface = packed >> 52
            return dict(index=index, parent=(row[0] >> 19) & MASK19, declaring=(row[0] >> 38) & MASK19,
                        kind=(row[8] >> 26) & 7, rank=row[8] >> 29, flags=row[2], instance=row[3],
                        hash=row[4], crc=row[5], impl=impl_index, ctor=row[6] & 0x3fffff,
                        method_start=(row[6] >> 22) & 0x3fffff, field_start=row[6] >> 44,
                        property_count=row[7] & 0xfff, field_size=imp[2], static_size=imp[3],
                        managed_count=(imp[4] >> 17) & 0xffff, field_count=(imp[4] >> 33) & 0xffffff,
                        attributes_id=imp[4] & 0x1ffff, module_id=imp[4] >> 57,
                        method_count=imp[5], native_count=imp[6], interface_id=imp[7], unknown1E=imp[8][:2].hex(),
                        element=(row[1] >> 19) & MASK19, array=row[1] & MASK19,
                        interfaces=packed & 0x3ffffff, generic=(packed >> 26) & 0x3ffffff,
                        interface_index=iface - 4096 if iface & 2048 else iface,
                        type_info=row[10], object_info=row[11], compatibility_key=row[12], slots=row[8] & 0x3ffffff)

        @functools.lru_cache(None)
        def name(index):
            if not index:
                return 'System.Void'
            t = typ(index)
            imp = impls[t['impl']]
            if t['kind'] == 2 and t['element']:
                return name(t['element']) + '[' + ',' * max(0, t['rank'] - 1) + ']'
            n = text(imp[0])
            if t['generic']:
                g = S.unpack_from('<I', tables['bytes'], t['generic'])[0]
                if g & MASK19 != index:
                    count = (g >> 19) & 127
                    ids = S.unpack_from(f'<{count}I', tables['bytes'], t['generic'] + 4)
                    n += '<' + ','.join(name(j) for j in ids) + '>'
            return (name(t['declaring']) + '.' if t['declaring'] else text(imp[1]) + '.' if text(imp[1]) else '') + n

        def generic_def(index):
            g = typ(index)['generic']
            return (S.unpack_from('<I', tables['bytes'], g)[0] & MASK19) if g else 0

        def interfaces(index):
            offset = typ(index)['interfaces']
            if not offset:
                return []
            count = S.unpack_from('<I', tables['bytes'], offset)[0]
            return list(S.unpack_from(f'<{count}I', tables['bytes'], offset + 4))

        @functools.lru_cache(None)
        def slots(index):
            t = typ(index)
            return list(S.unpack_from(f"<{t['managed_count']}I", tables['bytes'], t['slots']))

        @functools.lru_cache(None)
        def bitmap(index, offset):
            t = typ(index)
            words = (t['field_size'] + 511) // 512
            if not words:
                return ()
            if words == 1:
                return (S.unpack_from('<Q', tables['impls'], t['impl'] * 48 + offset)[0],)
            pointer = S.unpack_from('<Q', tables['impls'], t['impl'] * 48 + offset)[0]
            return tuple(process.qwords(pointer, words)) if pointer else (0,) * words

        def field(index):
            value = S.unpack_from('<Q', tables['fields'], index * 8)[0]
            impl_index = (value >> 19) & MASK19
            attributes, flags, offset, n = S.unpack_from('<HHII', tables['fimpls'], impl_index * 12)
            return dict(index=index, declaring=value & MASK19, type=(value >> 38) & MASK19,
                        attributes=attributes, flags=flags >> 1, offset=offset & 0x3ffffff, name=text(n & 0xfffffff))

        def own_fields(index):
            t = typ(index)
            return [field(i) for i in range(t['field_start'], t['field_start'] + t['field_count'])]

        def value_size(index):
            return 8 if 1 <= typ(index)['kind'] <= 4 else typ(index)['field_size']

        def canonical(value):
            return re.sub(r'`\d+', '', value)

        def param(index):
            attr, init, nm, value = S.unpack_from('<HHII', tables['params'], index * 12)
            return dict(type=value & MASK19, modifier=nm >> 30, flags=value >> 19, attributes=attr,
                        init=init, name=text(nm & 0x3fffffff) if nm & 0x3fffffff else '')

        @functools.lru_cache(None)
        def method(index):
            a, b, encoded = S.unpack_from('<IIi', tables['methods'], index * 12)
            attr, vi, flags, iflags, nm = S.unpack_from('<HhHHI', tables['mimpls'], (b & MASK19) * 12)
            offset = (a >> 19) | ((b >> 19) << 13)
            count, invoke, ret = S.unpack_from('<HHI', tables['bytes'], offset)
            ids = S.unpack_from(f'<{count}I', tables['bytes'], offset + 8)
            mask = original_parameter_mask if index < original_methods else 0xffffffff
            return dict(index=index, declaring=a & MASK19, name=text(nm & 0xfffffff), attributes=attr,
                        virtual_index=vi, flags=flags, impl_flags=iflags, count=count, invoke=invoke,
                        result=param(ret), parameters=[param(i & mask) for i in ids],
                        entry=method_base + encoded if encoded else 0)

        def abi(p, result=False):
            if p['modifier']:
                return ('byref', p['modifier'], p['flags'] & 3)
            t = typ(p['type'])
            if 1 <= t['kind'] <= 4:
                return ('pointer',)
            n = name(p['type'])
            if n == 'System.Void':
                return ('void',)
            if n in ('System.Single', 'System.Double'):
                return ('float', t['field_size'])
            size = t['field_size']
            return ('aggregate', size) if result and size not in (1, 2, 4, 8) else ('integer', size)

        def abi_key(m):
            return (bool(m['flags'] & 0x10), abi(m['result'], True), tuple(abi(p) for p in m['parameters']))

        groups = collections.defaultdict(list)
        generic_groups = collections.defaultdict(list)
        generic_parent_groups = collections.defaultdict(list)
        for index in range(1, original_count):
            r = rows[index]
            groups[((r[0] >> 19) & MASK19, (r[8] >> 26) & 7, bool(r[2] & 0x80))].append(index)
            g = (r[9] >> 26) & 0x3ffffff
            if g:
                generic_groups[S.unpack_from('<I', tables['bytes'], g)[0] & MASK19].append(index)
            parent = (r[0] >> 19) & MASK19
            if parent:
                parent_generic = (rows[parent][9] >> 26) & 0x3ffffff
                if parent_generic:
                    definition = S.unpack_from('<I', tables['bytes'], parent_generic)[0] & MASK19
                    generic_parent_groups[(definition, (r[8] >> 26) & 7, bool(r[2] & 0x80))].append(index)

        def nearest_original(index):
            while index >= original_count:
                index = typ(index)['parent']
            return index

        def controls(index):
            t = typ(index)
            definition = generic_def(index)
            pool = generic_groups.get(definition, []) if definition and definition < original_count else []
            if not pool and t['parent']:
                pool = generic_parent_groups.get((generic_def(t['parent']), t['kind'], bool(t['flags'] & 0x80)), [])
            if not pool:
                pool = groups.get((nearest_original(t['parent']), t['kind'], bool(t['flags'] & 0x80)), [])
            if not pool:
                root = nearest_original(t['parent'])
                pool = [j for base in groups.get((root, t['kind'], True), [])
                        for j in groups.get((base, t['kind'], bool(t['flags'] & 0x80)), [])]
            if t['kind'] == 2:
                pool = [j for j in pool if typ(j)['rank'] == t['rank'] and typ(typ(j)['element'])['kind'] == typ(t['element'])['kind']]
            if not pool:
                pool = [nearest_original(t['parent'])]
            def score(j):
                r = rows[j]
                imp = impls[(r[1] >> 38) & 0x3ffff]
                return (abs(((r[8] >> 26) & 7) - t['kind']) * 10000 +
                        bool((r[2] ^ t['flags']) & 0x10000000) * 10000 +
                        abs(((imp[4] >> 33) & 0xffffff) - t['field_count']) * 8 +
                        abs(imp[5] - t['method_count']) * 4 + abs(imp[2] - t['field_size']) / 8)
            return sorted((j for j in pool if j != index), key=score)[:3]

        @functools.lru_cache(None)
        def runtime(index):
            t = typ(index)
            out = {}
            if t['type_info']:
                raw = process.read(t['type_info'], 0x88)
                out['type_info_flags'] = S.unpack_from('<I', raw, 12)[0]
                out['type_info_size'] = S.unpack_from('<I', raw, 48)[0]
                out['type_info_parent'] = S.unpack_from('<Q', raw, 64)[0]
                out['type_info_definition'] = S.unpack_from('<Q', raw, 96)[0]
                out['sequence'] = S.unpack_from('<Q', raw, 104)[0]
                out['native_ancestor'] = S.unpack_from('<Q', raw, 120)[0]
            if t['object_info']:
                managed, prefix, definition = process.qwords(t['object_info'] - 16, 3)
                gap = managed - t['object_info']
                out.update(field_prefix=prefix, definition=definition, native_count=gap // 8 if gap >= 0 and gap % 8 == 0 else -1)
                out['managed_entries'] = process.qwords(managed, t['managed_count'])
                out['managed_address'] = managed
                out['native_entries'] = process.qwords(t['object_info'] + 8, max(0, t['native_count'] - 1))
                entries = interfaces(index)
                prefix_count = max((typ(v & MASK19)['interface_index'] for v in entries), default=-1) + 1
                out['interface_prefix_count'] = prefix_count
                out['interface_prefix'] = process.qwords(t['object_info'] - 16 - prefix_count * 8, prefix_count)
            return out

        findings = []
        check_counts = collections.Counter()
        def check(condition, category, index, detail):
            check_counts[category] += 1
            if not condition:
                findings.append(dict(category=category, type=name(index), index=index, detail=detail))

        def compact(index):
            t = typ(index)
            out = {k: t[k] for k in ('index', 'kind', 'flags', 'instance', 'field_size', 'static_size', 'field_count', 'method_count',
                   'managed_count', 'native_count', 'attributes_id', 'module_id', 'unknown1E', 'interface_id', 'interface_index', 'compatibility_key', 'property_count')}
            out.update(name=name(index), parent=name(t['parent']))
            r = runtime(index)
            out['runtime'] = {k: v for k, v in r.items() if k not in ('native_entries', 'managed_entries')}
            return out

        records = []
        custom_methods = []
        for index in range(original_count, final_count):
            t = typ(index)
            r = runtime(index)
            fields = own_fields(index)
            declaration = declarations.get(name(index), {})
            check((rows[index][0] & MASK19) == index, 'type_index', index, 'record index mismatch')
            check(t['impl'] >= S.unpack_from('<I', baseline_header, 28)[0], 'private_impl', index, 'new class reuses original impl')
            if declaration:
                check(bool(t['flags'] & 0x80) == bool(declaration.get('abstract', False)), 'declared_abstract', index, 'TOML vs metadata')
                if not declaration.get('generic_template') and not declaration.get('generic_definition'):
                    expected_fields = declaration.get('fields', [])
                    check(len(fields) == len(expected_fields), 'declared_field_count', index, 'TOML vs metadata')
                    for actual, requested in zip(fields, expected_fields):
                        check(actual['name'] == requested['name'] and canonical(name(actual['type'])) == canonical(requested['type']) and bool(actual['flags'] & 0x4000) == bool(requested.get('serialize', False)),
                              'declared_field', index, requested['name'])
            if t['flags'] & 0x08000000:
                check(not t['object_info'], 'open_generic_shape', index, 'open generic has an ObjectInfo')
            elif t['kind'] == 1:
                check(bool(t['type_info']), 'type_info_presence', index, 'closed class lacks TypeInfo')
                if not t['flags'] & 0x80:
                    check(bool(t['object_info']), 'object_info_presence', index, 'concrete class lacks ObjectInfo')
                if r:
                    expected_type = S.unpack_from('<Q', header, 104)[0] + index * 80
                    check(r.get('type_info_definition') == expected_type, 'type_info_backref', index, str(r.get('type_info_definition')))
                    check(r['type_info_parent'] == typ(t['parent'])['type_info'], 'type_info_parent', index, name(t['parent']))
                    check(bool(r['type_info_flags'] & 1) == bool(t['flags'] & 0x80), 'abstract_runtime_flag', index, str(r['type_info_flags']))
                    if any(f['flags'] & 0x4000 for f in fields):
                        check(bool(r['sequence']), 'serialized_sequence_presence', index, 'serialized own fields lack engine sequence')
            if t['kind'] == 1 and not t['flags'] & 0x08000000:
                intervals = sorted((f['offset'], f['offset'] + value_size(f['type']), f['name']) for f in fields if not f['flags'] & 0x10)
                for before, after in zip(intervals, intervals[1:]):
                    check(before[1] <= after[0], 'own_field_overlap', index, f'{before[2]} / {after[2]}')
                expected = {}
                for offset in (32, 40):
                    words = list(bitmap(t['parent'], offset)) if t['parent'] else []
                    words += [0] * max(0, (t['field_size'] + 511) // 512 - len(words))
                    for f in fields:
                        if f['flags'] & 0x10:
                            continue
                        size = value_size(f['type'])
                        check(f['offset'] + size <= t['field_size'], 'field_bounds', index, f['name']) if offset == 32 else None
                        check(f['offset'] >= typ(t['parent'])['field_size'], 'parent_field_overlap', index, f['name']) if offset == 32 else None
                        if 1 <= typ(f['type'])['kind'] <= 4:
                            bits = [0] if offset == 40 and typ(f['type'])['kind'] == 3 else [1]
                        else:
                            bits = bitmap(f['type'], offset)
                        for wi, bitword in enumerate(bits):
                            while bitword:
                                low = bitword & -bitword
                                bit = f['offset'] // 8 + wi * 64 + low.bit_length() - 1
                                words[bit // 64] |= 1 << (bit % 64)
                                bitword ^= low
                    expected[offset] = tuple(words)
                    check(bitmap(index, offset) == tuple(words), 'retain_bitmap' if offset == 32 else 'cycle_bitmap', index, 'independent field-type reconstruction')
            if t['object_info']:
                expected_type = S.unpack_from('<Q', header, 104)[0] + index * 80
                check(r['definition'] == expected_type, 'object_info_backref', index, str(r['definition']))
                check(r['native_count'] == t['native_count'], 'native_entry_count', index, f"metadata={t['native_count']},runtime={r['native_count']}")
                if t['kind'] == 1:
                    check(t['instance'] == r['field_prefix'] + t['field_size'], 'instance_size', index, 'instance != prefix + fields')
                check(len(slots(index)) == len(r['managed_entries']), 'managed_entry_count', index, 'metadata/runtime managed count')
                ancestor = t['parent']
                while ancestor and not typ(ancestor)['object_info']:
                    ancestor = typ(ancestor)['parent']
                inherited_mismatches = []
                own_mismatches = []
                if ancestor:
                    ar = runtime(ancestor)
                    if t['kind'] == 1:
                        # CLR builder rounds the native ancestor's size to 16B;
                        # via.Behavior is 72B, while its managed descendants start fields at 80B.
                        native_size = S.unpack('<I', process.read(r['native_ancestor'] + 48, 4))[0]
                        expected_prefix = (native_size + 15) & ~15
                        check(r['field_prefix'] == expected_prefix, 'native_field_prefix', index, f"native_size={native_size},expected={expected_prefix},actual={r['field_prefix']}")
                    aid = slots(ancestor)
                    for slot, mid in enumerate(slots(index)):
                        if mid >= original_methods:
                            m = method(mid)
                            if r['managed_entries'][slot] != m['entry']:
                                own_mismatches.append(slot)
                        elif slot < len(aid) and aid[slot] == mid and r['managed_entries'][slot] != ar['managed_entries'][slot]:
                            inherited_mismatches.append(slot)
                check(not own_mismatches, 'custom_virtual_entries', index, own_mismatches)
                check(not inherited_mismatches, 'inherited_virtual_entries', index, inherited_mismatches)
                prefix_bad = []
                for encoded in interfaces(index):
                    interface_index = typ(encoded & MASK19)['interface_index']
                    expected = r['managed_address'] + (encoded >> 19) * 8
                    actual = process.qword(t['object_info'] - 24 - interface_index * 8)
                    if actual != expected:
                        prefix_bad.append(dict(interface=name(encoded & MASK19), interface_index=interface_index, expected=expected, actual=actual))
                check(not prefix_bad, 'interface_prefix_mapping', index, prefix_bad)
            for mi in range(t['method_start'], t['method_start'] + t['method_count']):
                m = method(mi)
                custom_methods.append(m)
                check(m['declaring'] == index, 'method_owner', index, m['name'])
                check(bool(m['flags'] & 0x400) or bool(m['entry']), 'method_entry', index, m['name'])
                check(bool(m['invoke']), 'invoke_id', index, m['name'])
                check(m['count'] == len(m['parameters']), 'parameter_count', index, m['name'])
                requested_methods = [x for x in declaration.get('methods', []) if x['name'] == m['name']]
                if requested_methods:
                    def signature_matches(requested, include_names):
                        parameters = requested.get('parameters', [])
                        if canonical(requested.get('return_type', 'System.Void')) != canonical(name(m['result']['type'])) or len(parameters) != m['count']:
                            return False
                        for actual, expected in zip(m['parameters'], parameters):
                            modifier = expected.get('modifier', '')
                            if (include_names and actual['name'] != expected['name']) or canonical(name(actual['type'])) != canonical(expected['type']):
                                return False
                            if actual['modifier'] != (2 if modifier in ('ref', 'out') else 0):
                                return False
                            if modifier == 'out' and actual['flags'] & 3 != 2 or modifier == 'ref' and actual['flags'] & 3 > 1:
                                return False
                        return True
                    check(any(signature_matches(x, False) for x in requested_methods), 'declared_method_signature', index, m['name'])
                    check(any(signature_matches(x, True) for x in requested_methods), 'declared_parameter_names', index, m['name'])
                if m['virtual_index'] >= 0:
                    vi = m['virtual_index']
                    check(vi < t['managed_count'] and slots(index)[vi] == mi, 'virtual_index_identity', index, m['name'])
            peers = controls(index)
            rec = compact(index)
            rec.update(manifest=declaration.get('manifest'), peers=[compact(j) for j in peers],
                       fields=[{**f, 'type_name': name(f['type'])} for f in fields],
                       retain_bitmap=[hex(v) for v in bitmap(index, 32)], cycle_bitmap=[hex(v) for v in bitmap(index, 40)])
            if peers:
                peer = typ(peers[0])
                keys = ('flags', 'attributes_id', 'module_id', 'unknown1E', 'interface_id', 'native_count', 'property_count')
                rec['differences'] = {k: [t[k], peer[k]] for k in keys if t[k] != peer[k]}
                pr = runtime(peers[0])
                rec['runtime_differences'] = {k: [r.get(k), pr.get(k)] for k in ('type_info_flags', 'type_info_size', 'field_prefix', 'native_count') if r.get(k) != pr.get(k)}
            records.append(rec)

        wanted_invokes = {m['invoke'] for m in custom_methods}
        wanted_keys = collections.defaultdict(set)
        for m in custom_methods:
            wanted_keys[m['invoke']].add(repr(abi_key(m)))
        recipes = collections.defaultdict(dict)
        for mi in range(1, original_methods):
            a, b, encoded = S.unpack_from('<IIi', tables['methods'], mi * 12)
            po = (a >> 19) | ((b >> 19) << 13)
            count, invoke = S.unpack_from('<HH', tables['bytes'], po)
            if invoke not in wanted_invokes:
                continue
            if wanted_keys[invoke] <= recipes[invoke].keys():
                continue
            m = method(mi)
            key = repr(abi_key(m))
            if key not in recipes[invoke]:
                recipes[invoke][key] = dict(owner=name(m['declaring']), name=m['name'], method=mi,
                    flags=m['flags'], impl_flags=m['impl_flags'], abi=key)
        method_records = []
        for m in custom_methods:
            key = repr(abi_key(m))
            check(key in recipes[m['invoke']], 'invoke_abi', m['declaring'], dict(method=m['name'],invoke=m['invoke'],abi=key))
            spec = next((x for x in declarations.get(name(m['declaring']), {}).get('methods', []) if x['name'] == m['name']), {})
            semantic_peer = None
            ancestor = typ(m['declaring'])['parent']
            while ancestor and semantic_peer is None:
                at = typ(ancestor)
                for mi in range(at['method_start'], at['method_start'] + at['method_count']):
                    am = method(mi)
                    if am['name'] == m['name'] and abi_key(am) == abi_key(m):
                        semantic_peer = dict(owner=name(ancestor), name=am['name'], flags=am['flags'], impl_flags=am['impl_flags'], attributes=am['attributes'], method=mi)
                        break
                ancestor = at['parent']
            method_records.append({**m, 'owner': name(m['declaring']), 'result': {**m['result'], 'type_name': name(m['result']['type'])},
                'parameters': [{**p,'type_name':name(p['type'])} for p in m['parameters']], 'declared_kind':spec.get('kind'),
                'recipe':recipes[m['invoke']].get(key), 'abi':key, 'semantic_peer':semantic_peer})

        # A moving process/build must not silently produce a mixed-generation report.
        if process.read(tdb_address, 240) != header:
            raise RuntimeError('TDB header changed during capture; discard mixed-generation sample')
        host = Path(re.search(r'Bootstrap loaded:.*module=(.*)', log)[1].strip())
        high_groups = collections.defaultdict(list)
        for index in range(1, original_count):
            high_groups[(typ(index)['kind'], typ(index)['module_id'])].append(index)
        original_unknown_high = [{ 'kind':kind, 'value':high, 'count':len(ids), 'examples':[
            {k:compact(i)[k] for k in ('name','flags','field_count','method_count','static_size','module_id')}
            for i in ids[:3]]} for (kind,high),ids in high_groups.items()]
        semantic_gaps = []
        for m in method_records:
            peer = m['semantic_peer']
            if peer and peer['flags'] & 0x800 and not m['flags'] & 0x800:
                semantic_gaps.append(dict(category='lost_special_name', owner=m['owner'], method=m['name'], actual=hex(m['flags']), parent=hex(peer['flags'])))
            if m['name'] != '.ctor' and '.' in m['name'] and m['flags'] & 7 == 6:
                semantic_gaps.append(dict(category='explicit_interface_public_metadata', owner=m['owner'], method=m['name'], actual=hex(m['flags'])))
        exe = host.parents[2] / 'MonsterHunterWilds.exe'
        with exe.open('rb') as stream:
            pe_offset = S.unpack_from('<I', stream.read(64), 60)[0]
            stream.seek(pe_offset + 8)
            pe_timestamp = S.unpack('<I', stream.read(4))[0]
        source_names = (
            'include/ci/Provider.h', 'include/ci/Provider.hpp', 'include/ClassInjector.h',
            'src/TypeDatabaseRecords.h', 'src/CustomTypeConfig.h', 'src/CustomTypeBuildSpec.h',
            'src/TypeDatabase.h', 'src/TypeDatabase.cpp',
            'src/TypeDatabaseBuilder.h', 'src/TypeDatabaseBuilder.cpp',
            'src/ClassFields.h', 'src/ClassFields.cpp',
            'src/ClassMethods.h', 'src/ClassMethods.cpp',
            'src/GenericTypes.h', 'src/GenericTypes.cpp',
            'src/EnumMembers.h', 'src/EnumMembers.cpp',
            'src/ClassProviderContext.h', 'src/ClassProviderContext.cpp',
            'src/InvokeAdapters.h', 'src/InvokeAdapters.cpp',
            'src/MethodTrampolines.h', 'src/MethodTrampolines.cpp',
            'src/ClassProviders.h', 'src/ClassProviders.cpp',
            'src/ProviderConfig.h', 'src/ProviderConfig.cpp',
            'src/ClassVirtualTable.h', 'src/ClassVirtualTable.cpp',
            'src/TypeInjection.h', 'src/TypeInjection.cpp')
        source_paths = [Path('plugins/ClassInjector') / name for name in source_names]
        source_paths.append(Path('common/include/GameTypes.h'))
        sources = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_paths}
        report = dict(pid=pid, tdb=hex(tdb_address), version=81, original_types=original_count, custom_types=final_count-original_count,
            original_methods=original_methods, custom_methods=len(custom_methods), parameter_mask=hex(original_parameter_mask),
            host_sha256=hashlib.sha256(host.read_bytes()).hexdigest(), metadata_tables_sha256=hashlib.sha256(tables['types']).hexdigest(),
            exe_timestamp=pe_timestamp, exe_size=exe.stat().st_size, sources_sha256=sources,
            checks=dict(check_counts), findings=findings, types=records, methods=method_records, original_modules=original_unknown_high, semantic_gaps=semantic_gaps,
            limitations=['Read-only structural/ABI audit, not execution of every method or resource lifecycle.',
                         'Peer candidates are structural controls, not asserted gameplay-semantic equivalents.',
                         'Unknown bytes and flags require consumer analysis before classifying as missing behavior.'])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k:report[k] for k in ('pid','original_types','custom_types','custom_methods','checks')}, ensure_ascii=False))
        print('FINDINGS', json.dumps(collections.Counter(f['category'] for f in findings),ensure_ascii=False))
        print('OUTPUT', str(args.output))


if __name__ == '__main__':
    main()
