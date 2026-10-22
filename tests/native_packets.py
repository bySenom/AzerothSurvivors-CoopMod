"""Small inspection decoder, transcribed from the pinned original engine protocol source.

reference-source/crates/benilla-protocol/src/messages/{update_object,spells,combat_log}.
It rejects unknown shapes rather than using byte-pattern guesses.
"""
import struct,zlib

class Reader:
    def __init__(self,data):self.data=data;self.at=0
    def take(self,n):
        if n<0 or self.at+n>len(self.data):raise ValueError('truncated native packet')
        out=self.data[self.at:self.at+n];self.at+=n;return out
    def unpack(self,fmt):return struct.unpack('<'+fmt,self.take(struct.calcsize('<'+fmt)))
    def u8(self):return self.take(1)[0]
    def u32(self):return self.unpack('I')[0]
    def guid(self):
        mask=self.u8();return sum(self.u8()<<(i*8) for i in range(8) if mask&(1<<i))

def movement(r):
    update=r.u8();position=None
    if update&0x20:
        flags=r.u32();r.take(4);position=r.unpack('4f')
        if flags&0x02000000:r.take(24)
        if flags&0x200000:r.take(4)
        r.take(4)
        if flags&0x2000:r.take(16)
        if flags&0x4000000:r.take(4)
        r.take(24)
        if flags&0x400000:
            spline=r.u32()
            if spline&0x40000:r.take(4)
            elif spline&0x20000:r.take(8)
            elif spline&0x10000:r.take(12)
            r.take(12);nodes=r.u32();r.take(nodes*12+12)
    elif update&0x40:position=r.unpack('4f')
    if update&8:r.take(4)
    if update&16:r.take(4)
    if update&4:r.guid()
    if update&2:r.take(4)
    return dict(update_flags=update,position=position)

def fields(r):
    masks=[r.u32() for _ in range(r.u8())];result={}
    for word,mask in enumerate(masks):
        for bit in range(32):
            if mask&(1<<bit):result[word*32+bit]=r.u32()
    return result

def objects(opcode,body):
    if opcode==0x1f6:
        length=struct.unpack_from('<I',body)[0];body=zlib.decompress(body[4:]);assert len(body)==length
    r=Reader(body);count=r.u32();r.u8();result=[]
    for _ in range(count):
        kind=r.u8();obj=dict(kind=kind)
        if kind in (0,1,2,3):obj['guid']=r.guid()
        if kind in (2,3):obj['object_type']=r.u8()
        if kind in (1,2,3):obj.update(movement(r))
        if kind in (0,2,3):obj['fields']=fields(r)
        elif kind in (4,5):obj['guids']=[r.guid() for _ in range(r.u32())]
        elif kind!=1:raise ValueError(f'unknown object kind {kind}')
        result.append(obj)
    assert r.at==len(body),(r.at,len(body))
    return result

def combat(opcode,body):
    r=Reader(body)
    if opcode in (0x131,0x132):
        item,caster=r.guid(),r.guid();return dict(kind='spell',actor=caster,spell=r.u32())
    if opcode==0x250:
        target,actor=r.guid(),r.guid();return dict(kind='damage',actor=actor,target=target,spell=r.u32(),damage=r.u32())
    if opcode==0x14a:
        r.u32();actor,target=r.guid(),r.guid();return dict(kind='melee',actor=actor,target=target,damage=r.u32())
    return None
