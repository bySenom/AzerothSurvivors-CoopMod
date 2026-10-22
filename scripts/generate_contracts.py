"""Emit signatures only for an explicitly inspected, exact SHA-256 adapter."""
from pathlib import Path
import argparse, hashlib, json
import pefile

ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--game',type=Path,required=True);args=ap.parse_args()
    data=args.game.read_bytes(); digest=hashlib.sha256(data).hexdigest()
    adapters=[json.loads(p.read_text(encoding='utf-8-sig')) for p in (ROOT/'compat').glob('*.json')]
    matches=[a for a in adapters if a['executableSha256']==digest and 'native' in a]
    if len(matches)!=1:raise SystemExit('No inspected native adapter for this executable. No signatures changed.')
    adapter=matches[0];native=adapter['native'];pe=pefile.PE(data=data,fast_load=True)
    out=ROOT/'src/contracts'/native['contractDirectory'];out.mkdir(parents=True,exist_ok=True)
    def signature(rva,size):
        rva=int(rva,0) if isinstance(rva,str) else rva
        offset=pe.get_offset_from_rva(rva)
        return ','.join(f'0x{x:02x}' for x in data[offset:offset+size])
    intro='// Generated from an inspected, SHA-256-verified game executable.\n#pragma once\n#include <array>\n#include <cstddef>\n#include <cstdint>\n'
    server=native['server']
    build=intro+'namespace azs {\n'+f'inline constexpr char game_sha256[]="{digest}";\n'
    build+=f'inline constexpr std::uintptr_t server_tick_rva={server["tick"]};\n'
    build+='inline constexpr std::array<unsigned char,32> server_tick_prologue={'+signature(server['tick'],32)+'};\n'
    for key in ('pending_connections_length','auth_connections_length','world_sessions_length','minimum_server_span'):
        build+=f'inline constexpr std::size_t {key}={server[key]};\n'
    (out/'build_contract.hpp').write_text(build+'}\n',encoding='utf-8')
    transport=intro+'namespace azs {\nstruct NativeEntry { std::uint32_t rva; std::array<unsigned char,24> bytes; };\n'
    for name,address in native['transport'].items():transport+=f'inline constexpr std::uint32_t {name}_rva={address};\n'
    transport+='inline constexpr NativeEntry transport_entries[]={\n'
    for address in native['transport'].values():transport+='{'+address+',{'+signature(address,24)+'}},\n'
    transport+='};\n'+f'inline constexpr std::uint32_t executable_timestamp=0x{pe.FILE_HEADER.TimeDateStamp:x};\n'
    transport+=f'inline constexpr std::uint32_t executable_image_size=0x{pe.OPTIONAL_HEADER.SizeOfImage:x};\n}}\n'
    (out/'transport_contract.hpp').write_text(transport,encoding='utf-8')
    gameplay=intro+'namespace azs::gameplay {\nstruct Entry { std::uint32_t rva; std::array<unsigned char,24> bytes; };\n'
    for name,address in native['gameplay'].items():gameplay+='inline constexpr Entry '+name+'={'+address+',{'+signature(address,24)+'}};\n'
    gameplay+='inline constexpr Entry all_entries[]={'+','.join(native['gameplay'])+'};\n'
    gameplay+=f'inline constexpr std::uintptr_t frame_serialize_return_rva={native["frameSerializeReturn"]};\n}}\n'
    (out/'gameplay_contract.hpp').write_text(gameplay,encoding='utf-8')
    lobby=intro+'namespace azs::lobby {\n'+f'inline constexpr std::uintptr_t pick_request_rva={native["picker"]};\n'
    lobby+='inline constexpr std::array<unsigned char,32> pick_request_prologue={'+signature(native['picker'],32)+'};\n}\n'
    (out/'lobby_contract.hpp').write_text(lobby,encoding='utf-8')
    print('Generated inspected native contracts: '+digest)

if __name__=='__main__':main()
