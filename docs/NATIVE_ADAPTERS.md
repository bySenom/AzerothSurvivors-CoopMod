# Exact native build adapters

dev.8 adds the inspected 140,514,816-byte Windows executable with SHA-256
dc10453398ceb398076f6d3b6ac7859470de2ff7ccd68ac1d00a02f9f1e5209a.
No official game version number was established for this file. Dependency
version strings inside the binary are not used as the game version.

The former 0.104.0 / 9845e5c7 adapter is preserved under bin/legacy and compiled
with AZS_LEGACY_BUILD. The main bin directory targets the new build. The backend
chooses the CLI and all four modules together, using the exact compatibility
hash. Each CLI rejects a different executable hash, and modules retain their
entry-byte checks. No unknown-build bypass is included.

## Current native boundary

The new server tick entry is RVA 0x58aee30, UI request dispatcher 0x58bd380,
command dispatcher 0x58b3d60, game tick 0x58fb890, game constructor 0x58bf6d0,
packet serializer 0x58c1650, and continuous frame serializer return 0x58b096e.
The inspected request table still maps tag 11 to picked publication and tag 12
to silent preview. Exact entry signatures are generated from the pinned binary.

The native movable game value grew from 0xa20 to 0xae0 bytes. Server world-list
pointer/count moved from 0xa68/0xa70 to 0xb28/0xb30; UI object/vtable moved from
0xfe8/0xff0 to 0x10c0/0x10c8. The game packet queue moved to 0x620..0x638, UI
events to 0x638..0x650, player position to 0x4d0, health to 0x4e8, run clock to
0xa4c and phase to 0xadf. The native world-session stride stays 0xb0 bytes.
src/layout.hpp keeps these offsets and shared value ranges per adapter.

The local investigation used unwind-bounded disassembly, instruction matching
that masks relative addresses but preserves field displacements, direct caller
inspection, and targeted Ghidra decompilation. Automatic similarity candidates
are findings to review, never sufficient authorization to generate an adapter.
For example, the correct stream destructor was distinguished from a similar
unrelated Rust monomorphization by its stream-close and Arc-drop callees.

## Verification and limits

The new executable passed startup from the installed package, with base,
transport, gameplay and lobby modules initialized. The previous build also
passed startup through its selected legacy modules. ZIP extraction, setup,
updater and lobby tests passed.

A loopback native host passed stream authentication, wrong-key rejection and
stream teardown. Two authenticated protocol clients exercised a 24-second
native simulation with separate hero GUIDs/positions, movement relay, both
native spell casters, both heroes in object packets, personal HUDs and upgrade
events. A separate regression checked that menu/preview sessions receive no
run events, logout stops updates and re-entry uses a new UI epoch.

These are original-native and protocol tests. A rendered two-PC run, real LAN,
human confirmation of the new build's Elwynn click, full class coverage,
revival, disconnect recovery and complete-run rewards remain unverified.

The ZIP and the signed automatic update carry both adapters. Game EXE/Data
files and existing character profiles are not included or patched.

dev.9 also bounds/retries native export discovery while the game's loader is
changing its module list. Required session exports are resolved before hooks
are enabled, and missing exports have specific diagnostic messages. This
addresses an intermittent startup failure observed during the dev.8 deployment
check. Six consecutive fresh native starts passed after the change; this is
startup evidence, not a full rendered co-op acceptance test.
