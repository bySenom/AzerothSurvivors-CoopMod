from pathlib import Path
import hashlib,struct
import pefile
import argparse
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--game',type=Path);args=parser.parse_args()
game=args.game or ROOT/'game/AzerothSurvivors.exe'
data=game.read_bytes()
assert hashlib.sha256(data).hexdigest()=='96e24dcd0e53fc13a6fdef5826aa46f77a29aa1aad4abd585adf04e94b2756d3'
pe=pefile.PE(data=data,fast_load=True)
addresses=[0x05878ce0,0x059b4170,0x059b42d0,0x00fdb740,0x059b5140,0x000021d0]
lines=['// Generated from the exact, hash-verified original executable.','#pragma once','#include <array>','#include <cstdint>',
       'namespace azs {','struct NativeEntry { std::uint32_t rva; std::array<unsigned char,24> bytes; };',
       'inline constexpr NativeEntry transport_entries[] = {']
for rva in addresses:
    offset=pe.get_offset_from_rva(rva)
    values=','.join(f'0x{v:02x}' for v in data[offset:offset+24])
    lines.append(f'    {{0x{rva:08x},{{{values}}}}},')
lines+=['};',f'inline constexpr std::uint32_t executable_timestamp = 0x{pe.FILE_HEADER.TimeDateStamp:08x};',
        f'inline constexpr std::uint32_t executable_image_size = 0x{pe.OPTIONAL_HEADER.SizeOfImage:08x};','}']
(ROOT/'src/transport_contract.hpp').write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('Generated exact-build native transport signatures')
