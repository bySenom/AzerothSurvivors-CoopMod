"""Read-only xrefs for native picker boundaries; findings do not authorize hooks."""
from pathlib import Path
import argparse,bisect,json,re,struct
import pefile
from iced_x86 import Decoder,Formatter,FormatterSyntax

ap=argparse.ArgumentParser()
ap.add_argument('--game',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
args=ap.parse_args()
data=args.game.read_bytes(); pe=pefile.PE(data=data,fast_load=True)
base=pe.OPTIONAL_HEADER.ImageBase
sections={s.Name.rstrip(b'\0').decode():s for s in pe.sections}
pd=sections['.pdata']; block=data[pd.PointerToRawData:pd.PointerToRawData+pd.Misc_VirtualSize]
functions=sorted((base+a,base+b) for a,b,_ in struct.iter_unpack('<III',block))
starts=[a for a,_ in functions]
needles={}
for name in ('WOW_ROGUE_GLUE','picker.enter','picker.entering','picker.controls_hint','picker.starts_with','picker.zones_open'):
    position=0
    while (position:=data.find(name.encode(),position))>=0:
        needles[base+pe.get_rva_from_offset(position)]=name
        position+=len(name)
text=sections['.text']; code=data[text.PointerToRawData:text.PointerToRawData+text.Misc_VirtualSize]
rows=[]
for m in re.finditer(rb'[\x40-\x4f][\x8b\x8d][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]',code):
    off=m.start()
    if off+7>len(code):continue
    ip=base+text.VirtualAddress+off
    address=ip+7+struct.unpack_from('<i',code,off+3)[0]
    if address not in needles:continue
    ins=Decoder(64,code[off:off+15],ip=ip).decode()
    if not ins.is_ip_rel_memory_operand or ins.ip_rel_memory_address!=address:continue
    i=bisect.bisect_right(starts,ip)-1
    if i<0 or ip>=functions[i][1]:continue
    first,last=functions[i]
    rows.append(dict(name=needles[address],instruction=hex(ip),function=hex(first),end=hex(last),size=last-first))
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(rows,indent=2),encoding='utf-8')
print(json.dumps(rows,indent=2))
