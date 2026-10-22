from pathlib import Path
import argparse,hashlib
import pefile

root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('--game',type=Path,required=True);args=ap.parse_args()
data=args.game.read_bytes()
if hashlib.sha256(data).hexdigest()!='96e24dcd0e53fc13a6fdef5826aa46f77a29aa1aad4abd585adf04e94b2756d3':
    raise SystemExit('Unsupported native picker build. Inspect the adapter before regenerating it.')
pe=pefile.PE(data=data,fast_load=True)
rva=0x05887c50;offset=pe.get_offset_from_rva(rva)
signature=','.join(f'0x{x:02x}' for x in data[offset:offset+32])
(root/'src/lobby_contract.hpp').write_text(
    '// Exact-build native UI request dispatcher. Tag 11 emits the picked event.\n'
    '#pragma once\n#include <array>\n#include <cstdint>\nnamespace azs::lobby {\n'
    f'inline constexpr std::uintptr_t pick_request_rva=0x{rva:x};\n'
    f'inline constexpr std::array<unsigned char,32> pick_request_prologue={{{signature}}};\n'
    '}\n',encoding='utf-8')
print('Generated native picker entry signature')
