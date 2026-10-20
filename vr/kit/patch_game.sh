#!/bin/sh
# Generated from vr/q0/game_patches.json by the kit build. Patches a copy of
# the pristine TP game binary in place with dd (POSIX/toybox only, no python).
# Usage: patch_game.sh <copy-of-pristine-game>
set -e
G="$1"
[ -f "$G" ] || { echo "usage: $0 <game copy>" >&2; exit 2; }
p() { printf "$2" | dd of="$G" bs=1 seek="$1" conv=notrunc 2>/dev/null; }
p 17429496 '\026\173\004\011'  # descriptor-slots: descriptor slot 0x90e73f8
p 17429508 '\106\173\004\011'  # descriptor-slots: descriptor slot 0x90e7404
p 17429516 '\146\173\004\011'  # descriptor-slots: descriptor slot 0x90e740c
p 17429544 '\326\173\004\011\346\173\004\011\366\173\004\011\006\174\004\011\026\174\004\011\046\174\004\011'  # descriptor-slots: descriptor slot 0x90e7428
p 17429580 '\146\174\004\011'  # descriptor-slots: descriptor slot 0x90e744c
p 17429612 '\346\174\004\011\366\174\004\011\006\175\004\011\026\175\004\011'  # descriptor-slots: descriptor slot 0x90e746c
p 17429632 '\066\175\004\011'  # descriptor-slots: descriptor slot 0x90e7480
p 17429648 '\166\175\004\011\206\175\004\011'  # descriptor-slots: descriptor slot 0x90e7490
p 17429664 '\266\175\004\011'  # descriptor-slots: descriptor slot 0x90e74a0
p 17429672 '\326\175\004\011'  # descriptor-slots: descriptor slot 0x90e74a8
p 17429680 '\366\175\004\011'  # descriptor-slots: descriptor slot 0x90e74b0
p 17429688 '\026\176\004\011'  # descriptor-slots: descriptor slot 0x90e74b8
p 17429720 '\226\176\004\011\246'  # descriptor-slots: descriptor slot 0x90e74d8
p 17429726 '\004\011\266\176\004\011'  # descriptor-slots: descriptor slot 0x90e74de
p 17429756 '\046\177\004\011'  # descriptor-slots: descriptor slot 0x90e74fc
p 17429768 '\126\177\004\011\146\177\004\011\166\177\004\011\206\177\004\011\226\177\004\011\246\177\004\011'  # descriptor-slots: descriptor slot 0x90e7508
p 17429804 '\346\177\004\011'  # descriptor-slots: descriptor slot 0x90e752c
p 17429812 '\006\200\004\011'  # descriptor-slots: descriptor slot 0x90e7534
p 17429828 '\106\200\004\011'  # descriptor-slots: descriptor slot 0x90e7544
p 17429844 '\206\200\004\011\226\200\004\011\246\200\004\011\266\200\004\011'  # descriptor-slots: descriptor slot 0x90e7554
p 17429896 '\126\201\004\011'  # descriptor-slots: descriptor slot 0x90e7588
p 162124 '\000'  # undefine-export: _ZTISt9exception
p 167980 '\000'  # undefine-export: inflateInit2_
p 177004 '\000'  # undefine-export: inflateReset
p 177180 '\000'  # undefine-export: main
p 184732 '\000'  # undefine-export: _ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE
p 206588 '\000'  # undefine-export: _ZTVN10__cxxabiv117__class_type_infoE
p 218044 '\000'  # undefine-export: adler32
p 218252 '\000'  # undefine-export: strcmp
p 221932 '\000'  # undefine-export: _IO_stdin_used
p 241532 '\000'  # undefine-export: deflateReset
p 258188 '\000'  # undefine-export: zlibVersion
p 263836 '\000'  # undefine-export: stdout
p 281164 '\000'  # undefine-export: _ZTVN10__cxxabiv120__si_class_type_infoE
p 282972 '\000'  # undefine-export: malloc
p 353740 '\000'  # undefine-export: glutBitmap8By13
p 361340 '\000'  # undefine-export: stderr
p 361660 '\000'  # undefine-export: inflate
p 375852 '\000'  # undefine-export: stdin
p 376636 '\000'  # undefine-export: deflate
p 388716 '\000'  # undefine-export: uncompress
p 398652 '\000'  # undefine-export: deflateInit2_
p 402060 '\000'  # undefine-export: __gxx_personality_v0
p 402556 '\000'  # undefine-export: deflateEnd
p 413788 '\000'  # undefine-export: free
p 434012 '\000'  # undefine-export: __cxa_pure_virtual
p 436620 '\000'  # undefine-export: compress
p 439884 '\000'  # undefine-export: inflateEnd
p 443516 '\000'  # undefine-export: _ZNSs4_Rep20_S_empty_rep_storageE
p 444188 '\000'  # undefine-export: crc32
p 451308 '\000'  # undefine-export: deflateInit_
p 452748 '\000'  # undefine-export: realloc
