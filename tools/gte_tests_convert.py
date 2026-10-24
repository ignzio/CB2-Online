"""Convert JaCzekanski/ps1-tests gte/test-all tests.c (real-hardware captures) into a flat binary for cb2_gtetest.

Layout: u32 count, then per test: u32 input[64], u32 opcode, u32 output[64].
Registers 0-31 are GTE data registers, 32-63 are control registers.
"""
import re
import struct
import sys

src = open(sys.argv[1], encoding="utf-8").read()
tests = []
for blk in re.split(r"\n\{ // test", src)[1:]:
    ins = re.search(r"\.input = \{(.*?)\n  \},", blk, re.S).group(1)
    outs = re.search(r"\.output = \{(.*?)\n  \}", blk, re.S).group(1)
    op = int(re.search(r"\.opcode = (0x[0-9a-fA-F]+)", blk).group(1), 16)
    iv = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8}),", ins)]
    ov = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})[, ]", outs)]
    assert len(iv) == 64 and len(ov) == 64, (len(iv), len(ov))
    tests.append((iv, op, ov))
with open(sys.argv[2], "wb") as f:
    f.write(struct.pack("<I", len(tests)))
    for iv, op, ov in tests:
        f.write(struct.pack("<64II64I", *iv, op, *ov))
print(len(tests), "tests;", sum(1 for t in tests if t[1] != 0xFFFFFFFF), "with opcodes")
