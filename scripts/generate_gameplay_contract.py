"""Generate entry signatures from the supported original, never from inferred source."""
from pathlib import Path
import hashlib
import pefile
import argparse

ROOT = Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--game',type=Path);args=parser.parse_args()
data = (args.game or ROOT / 'game/AzerothSurvivors.exe').read_bytes()
assert hashlib.sha256(data).hexdigest() == '96e24dcd0e53fc13a6fdef5826aa46f77a29aa1aad4abd585adf04e94b2756d3'
pe = pefile.PE(data=data, fast_load=True)
entries = [('serialize', 0x0588bc40), ('command', 0x0587e730),
           ('game_tick', 0x058feb20), ('construct', 0x05889fa0), ('destroy', 0x05877f50),
           ('addon', 0x0588e770), ('run_text', 0x058e58b0), ('stats_text', 0x058ed490),
           ('zone_text', 0x058e6740), ('boss_text', 0x058e92c0), ('units_text', 0x058e7bf0),
           ('debuffs_text', 0x058e84e0), ('target_text', 0x058e8090), ('spells_text', 0x058e7f60),
           ('buffs_text', 0x058e7630), ('chests_text', 0x058e7e20), ('lairs_text', 0x058e7750),
           ('taken_text', 0x058e78d0), ('end_text', 0x058e4d90), ('meter_text', 0x058b2120),
           ('write', 0x059b4170), ('free', 0x000021d0)]
lines = ['// Generated from the supported original executable.', '#pragma once',
         '#include <array>', '#include <cstdint>', 'namespace azs::gameplay {',
         'struct Entry { std::uint32_t rva; std::array<unsigned char,24> bytes; };']
for name, rva in entries:
    offset = pe.get_offset_from_rva(rva)
    signature = ','.join(f'0x{x:02x}' for x in data[offset:offset+24])
    lines.append(f'inline constexpr Entry {name} = {{0x{rva:x},{{{signature}}}}};')
lines += ['inline constexpr Entry all_entries[] = {' + ','.join(name for name,_ in entries) + '};',
          '// Only the continuous frame packet loop, excluding challenge/login/control sends.',
          'inline constexpr std::uintptr_t frame_serialize_return_rva = 0x0587b43e;', '}']
(ROOT / 'src/gameplay_contract.hpp').write_text('\n'.join(lines) + '\n', encoding='utf-8')
print('Generated native gameplay signatures')
