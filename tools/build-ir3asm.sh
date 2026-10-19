#!/bin/sh
# Build tools/ir3asm (Mesa's ir3 assembler + disassembler for the A530) on
# macOS without meson. Inputs: a sparse Mesa checkout in research/mesa
# (src/freedreno src/compiler src/util src/c11 include), bison >= 3.7 and a
# python with mako in research/tools (see NOTES.md, GPU STEP 4).
# Output: research/tools/bin/ir3asm
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
M=$R/research/mesa/src
T=$R/research/tools
P=$T/venv/bin/python
B=$T/ir3asm
G=$B/gen
mkdir -p "$G" "$B/obj" "$T/bin"

cd "$G"
$P $M/compiler/nir/nir_opcodes_h.py > nir_opcodes.h
$P $M/compiler/nir/nir_builder_opcodes_h.py > nir_builder_opcodes.h
$P $M/compiler/nir/nir_intrinsics_h.py --out nir_intrinsics.h
$P $M/compiler/nir/nir_intrinsics_indices_h.py --out nir_intrinsics_indices.h
$P $M/compiler/builtin_types_h.py builtin_types.h
$P $M/compiler/isaspec/decode.py --xml $M/freedreno/isa/ir3.xml --out-c ir3-isa.c --out-h ir3-isa.h
$P $M/compiler/isaspec/encode.py --xml $M/freedreno/isa/ir3.xml --out-h encode.h
$T/bin/bison $M/freedreno/ir3/ir3_parser.y --name-prefix=ir3_yy --defines=ir3_parser.h --output=ir3_parser.c
flex -o ir3_lexer.c $M/freedreno/ir3/ir3_lexer.l
mkdir -p util/format && $P $M/util/format/u_format_table.py $M/util/format/u_format.yaml --enums > util/format/u_format_gen.h

SRCS="$R/tools/ir3asm/main.c $R/tools/ir3asm/stubs.c $G/ir3_parser.c $G/ir3_lexer.c $G/ir3-isa.c
 $M/freedreno/ir3/ir3_assembler.c $M/freedreno/ir3/ir3.c $M/freedreno/isa/encode.c
 $M/compiler/isaspec/isaspec.c $M/util/ralloc.c $M/util/hash_table.c $M/util/set.c
 $M/util/u_debug.c $M/util/log.c $M/util/half_float.c $M/util/softfloat.c"
CFLAGS="-O1 -g -std=gnu11 -w -I$G -I$R/research/mesa/include -I$M -I$M/freedreno -I$M/freedreno/ir3
 -I$M/freedreno/common -I$M/freedreno/isa -I$M/compiler -I$M/compiler/nir -I$M/compiler/isaspec -I$M/util
 -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -DPACKAGE_VERSION=\"ir3asm\""
cc $CFLAGS -o $T/bin/ir3asm $SRCS
echo "built $T/bin/ir3asm"
