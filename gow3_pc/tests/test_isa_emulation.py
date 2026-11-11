"""isa_emulation.py against the real instructions: every emulation runs on this CPU next to the
instruction it replaces, from the same random registers, flags and memory, and must leave the same
general registers, ymm registers, defined flags, rsp and memory (the red zone below rsp included).

Needs Windows x64, a CPU with BMI1, LZCNT, MOVBE and F16C (Haswell or newer) and iced_x86 in
out/pydeps; skipped otherwise. TLOU_ISA_TEST_SEED and TLOU_ISA_TEST_CASES change the run.
"""
from paths import ROOT
import ctypes
import os
import random
import struct
import sys
import unittest

sys.path.insert(0, str(ROOT / 'out' / 'pydeps'))
try:
    from iced_x86 import BlockEncoder, Code as C, Encoder, Instruction as I, MemoryOperand as M, Register as R
except ImportError:
    I = None
import cpu_features
import isa_emulation

GPR64 = None if I is None else [R.RAX, R.RCX, R.RDX, R.RBX, R.RSP, R.RBP, R.RSI, R.RDI,
                                R.R8, R.R9, R.R10, R.R11, R.R12, R.R13, R.R14, R.R15]
GPR32 = None if I is None else [R.EAX, R.ECX, R.EDX, R.EBX, R.ESP, R.EBP, R.ESI, R.EDI,
                                R.R8D, R.R9D, R.R10D, R.R11D, R.R12D, R.R13D, R.R14D, R.R15D]
GPR16 = None if I is None else [R.AX, R.CX, R.DX, R.BX, R.SP, R.BP, R.SI, R.DI,
                                R.R8W, R.R9W, R.R10W, R.R11W, R.R12W, R.R13W, R.R14W, R.R15W]
XMM = None if I is None else [getattr(R, f'XMM{i}') for i in range(16)]
YMM = None if I is None else [getattr(R, f'YMM{i}') for i in range(16)]
NONVOLATILE = (3, 5, 6, 7, 12, 13, 14, 15)          # rbx rbp rsi rdi r12-r15 (Windows x64)

# Block layout (offsets from the block's start).
GPRS, FLAGS, TMP, SAVE_RSP, SAVE = 0, 128, 136, 144, 152
YMMS, SAVEX, GUEST = 256, 768, 1280
DATA, DATA_SIZE = 2048, 1024
STACK, STACK_SIZE, GUEST_OFF = 4096, 4096, 3072      # guest rsp = block + STACK + GUEST_OFF
BLOCK_SIZE = 8192
REGION, CODE_SIZE, BLOCK_AT = 65536, 16384, 32768

CF, PF, AF, ZF, SF, OF = 1, 4, 16, 64, 128, 2048
ARITH = CF | PF | AF | ZF | SF | OF
DEFINED = {          # flags each instruction defines (the others are undefined and not compared)
    'tzcnt': CF | ZF, 'lzcnt': CF | ZF, 'andn': SF | ZF | CF | OF, 'bextr': ZF | CF | OF,
    'blsr': SF | ZF | CF | OF, 'blsmsk': SF | ZF | CF | OF, 'blsi': SF | ZF | CF | OF,
    'movbe_load': ARITH, 'movbe_store': ARITH, 'cvtph2ps': ARITH, 'cvtps2ph': ARITH,
}


def supported():
    if I is None or sys.platform != 'win32' or struct.calcsize('P') != 8:
        return False
    return cpu_features.missing_emulatable() == [] and not cpu_features.missing_blocking()


class Machine:
    """An RWX region holding a harness (code) and the block it loads and stores."""

    def __init__(self):
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.VirtualAlloc.restype = ctypes.c_void_p
        k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
        self.base = k32.VirtualAlloc(None, REGION, 0x3000, 0x40)
        if not self.base:
            raise OSError('VirtualAlloc failed')
        self.block = self.base + BLOCK_AT
        self.guest_rsp = self.block + STACK + GUEST_OFF

    def _encode(self, instructions, ip):
        encoder = BlockEncoder(64)
        encoder.add_many(instructions)
        return encoder.encode(ip)

    def prologue(self):
        b = self.block
        out = [I.create_reg_u64(C.MOV_R64_IMM64, R.RAX, b),
               I.create_mem_reg(C.MOV_RM64_R64, M(R.RAX, displ=SAVE_RSP, displ_size=8), R.RSP)]
        for k, n in enumerate(NONVOLATILE):
            out.append(I.create_mem_reg(C.MOV_RM64_R64, M(R.RAX, displ=SAVE + 8 * k, displ_size=8), GPR64[n]))
        for n in range(6, 16):
            out.append(I.create_mem_reg(C.VEX_VMOVDQU_YMMM256_YMM, M(R.RAX, displ=SAVEX + 32 * n, displ_size=8), YMM[n]))
        for n in range(16):
            out.append(I.create_reg_mem(C.VEX_VMOVDQU_YMM_YMMM256, YMM[n], M(R.RAX, displ=YMMS + 32 * n, displ_size=8)))
        out.append(I.create_mem(C.PUSH_RM64, M(R.RAX, displ=FLAGS, displ_size=8)))
        out.append(I.create(C.POPFQ))
        out.append(I.create_reg_mem(C.MOV_R64_RM64, R.RSP, M(R.RAX, displ=GUEST, displ_size=8)))
        for n in range(1, 16):
            if n != 4:
                out.append(I.create_reg_mem(C.MOV_R64_RM64, GPR64[n], M(R.RAX, displ=GPRS + 8 * n, displ_size=8)))
        out.append(I.create_reg_mem(C.MOV_R64_RM64, R.RAX, M(R.RAX, displ=GPRS, displ_size=8)))
        return out

    def epilogue(self):
        b = self.block
        out = [I.create_mem_reg(C.MOV_MOFFS64_RAX, M(displ=b + TMP, displ_size=8), R.RAX),
               I.create_reg_u64(C.MOV_R64_IMM64, R.RAX, b)]
        for n in range(1, 16):
            out.append(I.create_mem_reg(C.MOV_RM64_R64, M(R.RAX, displ=GPRS + 8 * n, displ_size=8), GPR64[n]))
        out.append(I.create_reg_mem(C.MOV_R64_RM64, R.RCX, M(R.RAX, displ=TMP, displ_size=8)))
        out.append(I.create_mem_reg(C.MOV_RM64_R64, M(R.RAX, displ=GPRS, displ_size=8), R.RCX))
        out.append(I.create_reg_mem(C.MOV_R64_RM64, R.RSP, M(R.RAX, displ=SAVE_RSP, displ_size=8)))
        out.append(I.create(C.PUSHFQ))
        out.append(I.create_reg(C.POP_R64, R.RCX))
        out.append(I.create_mem_reg(C.MOV_RM64_R64, M(R.RAX, displ=FLAGS, displ_size=8), R.RCX))
        for n in range(16):
            out.append(I.create_mem_reg(C.VEX_VMOVDQU_YMMM256_YMM, M(R.RAX, displ=YMMS + 32 * n, displ_size=8), YMM[n]))
        for n in range(6, 16):
            out.append(I.create_reg_mem(C.VEX_VMOVDQU_YMM_YMMM256, YMM[n], M(R.RAX, displ=SAVEX + 32 * n, displ_size=8)))
        for k, n in enumerate(NONVOLATILE):
            out.append(I.create_reg_mem(C.MOV_R64_RM64, GPR64[n], M(R.RAX, displ=SAVE + 8 * k, displ_size=8)))
        out.append(I.create(C.VEX_VZEROUPPER))
        out.append(I.create(C.RETNQ))
        return out

    def run(self, body, state):
        """body(ip) -> bytes placed at ip between the prologue and epilogue; state: bytes of the block.
        Returns the block after the run."""
        start = self.base
        head = self._encode(self.prologue(), start)
        code = body(start + len(head))
        tail = self._encode(self.epilogue(), start + len(head) + len(code))
        program = head + code + tail
        assert len(program) <= CODE_SIZE
        ctypes.memmove(start, bytes(program), len(program))
        ctypes.memmove(self.block, bytes(state), BLOCK_SIZE)
        ctypes.CFUNCTYPE(None)(start)()
        return ctypes.string_at(self.block, BLOCK_SIZE)


# ---------------------------------------------------------------- test cases

def interesting(rng, bits):
    mask = (1 << bits) - 1
    pick = rng.random()
    if pick < 0.15:
        return 0
    if pick < 0.3:
        return 1 << rng.randrange(bits)
    if pick < 0.4:
        return mask
    if pick < 0.5:
        return mask ^ (1 << rng.randrange(bits))
    return rng.getrandbits(64)              # upper bits too: 32/16-bit forms must handle them


def half_value(rng):
    pick = rng.random()
    if pick < 0.1:
        return rng.choice((0, 0x8000, 0x7c00, 0xfc00, 0x7e00, 0x7c01, 0x7bff, 0x0400, 0x03ff, 0x0001))
    if pick < 0.25:
        return rng.randrange(0x400) | rng.choice((0, 0x8000))         # subnormal
    if pick < 0.35:
        return 0x7c00 | rng.randrange(1, 0x400) | rng.choice((0, 0x8000))   # nan
    return rng.getrandbits(16)


def float_value(rng):
    pick = rng.random()
    if pick < 0.1:
        return rng.choice((0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0x477fe000,
                           0x477ff000, 0x477fefff, 0x47800000, 0x38800000, 0x387fffff, 0x33000000, 0x33000001))
    if pick < 0.3:
        return struct.unpack('<I', struct.pack('<f', rng.uniform(-70000, 70000)))[0]
    if pick < 0.45:
        return struct.unpack('<I', struct.pack('<f', rng.uniform(-1e-4, 1e-4)))[0]
    if pick < 0.6:                                # halfway cases between two halves (round to even)
        return (rng.randrange(0x38800000, 0x47800000) & ~0x1fff) | 0x1000 | rng.choice((0, 0x80000000))
    return rng.getrandbits(32)


class Case:
    def __init__(self, kind, instruction, gprs, ymms, flags, data, stack):
        self.kind, self.instruction = kind, instruction
        self.gprs, self.ymms, self.flags, self.data, self.stack = gprs, ymms, flags, data, stack


def make_case(rng, machine, kind):
    gprs = [rng.getrandbits(64) for _ in range(16)]
    ymms = [bytearray(rng.getrandbits(8) for _ in range(32)) for _ in range(16)]
    flags = 0x202 | sum(f for f in (CF, PF, AF, ZF, SF, OF) if rng.random() < 0.5)
    data = bytearray(rng.getrandbits(8) for _ in range(DATA_SIZE))
    stack = bytearray(rng.getrandbits(8) for _ in range(STACK_SIZE))
    vector = kind in ('cvtph2ps', 'cvtps2ph')
    size = 64 if vector else rng.choice((16, 32, 64) if kind in ('tzcnt', 'lzcnt', 'movbe_load', 'movbe_store') else (32, 64))
    regs = {64: GPR64, 32: GPR32, 16: GPR16}[size]
    usable = [n for n in range(16) if n != 4]
    taken = set()

    def reg():
        n = rng.choice(usable)
        taken.add(n)
        return n

    def memory(width):
        """A memory operand whose bytes lie in the data area (or above the guest rsp)."""
        form = rng.choice(('base', 'base', 'index', 'rsp', 'rip', 'r12', 'r13'))
        if form == 'rsp':
            disp = rng.randrange(0, 256 - width)
            return M(R.RSP, displ=disp, displ_size=1), None
        offset = rng.randrange(0, DATA_SIZE - width)
        target = machine.block + DATA + offset
        if form == 'rip':
            return M(R.RIP, displ=target, displ_size=8), None
        if form in ('r12', 'r13'):
            base = 12 if form == 'r12' else 13
            gprs[base] = target
            return M(GPR64[base], displ=0, displ_size=0), base
        base = rng.choice(usable)
        disp = rng.randrange(-100, 100)
        if form == 'index':
            index = rng.choice([n for n in usable if n != base])
            scale = rng.choice((1, 2, 4, 8))
            value = rng.randrange(0, 64)
            gprs[index] = value
            gprs[base] = (target - disp - value * scale) & (1 << 64) - 1
            taken.update((base, index))
            return M(GPR64[base], GPR64[index], scale, disp, 1), base
        gprs[base] = (target - disp) & (1 << 64) - 1
        taken.add(base)
        return M(GPR64[base], displ=disp, displ_size=1), base

    def value_for(n, bits=size):
        gprs[n] = interesting(rng, bits)

    use_mem = rng.random() < 0.4
    if kind in ('tzcnt', 'lzcnt'):
        code = getattr(C, f'{kind.upper()}_R{size}_RM{size}')
        dst = reg()
        if use_mem:
            mem, _ = memory(size // 8)
            ins = I.create_reg_mem(code, regs[dst], mem)
        else:
            src = reg()
            value_for(src)
            ins = I.create_reg_reg(code, regs[dst], regs[src])
    elif kind == 'movbe_load':
        dst = reg()
        mem, _ = memory(size // 8)
        ins = I.create_reg_mem(getattr(C, f'MOVBE_R{size}_M{size}'), regs[dst], mem)
    elif kind == 'movbe_store':
        src = reg()
        value_for(src)
        mem, _ = memory(size // 8)               # may reuse src as the base: allowed too
        ins = I.create_mem_reg(getattr(C, f'MOVBE_M{size}_R{size}'), mem, regs[src])
    elif kind == 'andn':
        dst, a = reg(), reg()
        value_for(a)
        code = getattr(C, f'VEX_ANDN_R{size}_R{size}_RM{size}')
        if use_mem:
            mem, _ = memory(size // 8)
            ins = I.create_reg_reg_mem(code, regs[dst], regs[a], mem)
        else:
            b = reg()
            value_for(b)
            ins = I.create_reg_reg_reg(code, regs[dst], regs[a], regs[b])
    elif kind == 'bextr':
        dst, ctrl = reg(), reg()
        start, length = rng.choice((rng.randrange(size), rng.randrange(256))), rng.choice((rng.randrange(size + 1), rng.randrange(256)))
        gprs[ctrl] = rng.getrandbits(48) << 16 | length << 8 | start
        code = getattr(C, f'VEX_BEXTR_R{size}_RM{size}_R{size}')
        if use_mem:
            mem, _ = memory(size // 8)
            ins = I.create_reg_mem_reg(code, regs[dst], mem, regs[ctrl])
        else:
            src = reg()
            if src != ctrl:
                value_for(src)
            ins = I.create_reg_reg_reg(code, regs[dst], regs[src], regs[ctrl])
    elif kind in ('blsr', 'blsmsk', 'blsi'):
        dst = reg()
        code = getattr(C, f'VEX_{kind.upper()}_R{size}_RM{size}')
        if use_mem:
            mem, _ = memory(size // 8)
            ins = I.create_reg_mem(code, regs[dst], mem)
        else:
            src = reg()
            value_for(src)
            ins = I.create_reg_reg(code, regs[dst], regs[src])
    elif kind == 'cvtph2ps':
        wide = rng.random() < 0.5
        dst = rng.randrange(16)
        halves = [half_value(rng) for _ in range(8)]
        packed = struct.pack('<8H', *halves)
        if use_mem:
            mem, _ = memory(16 if wide else 8)
            # the halves go where the operand points
            ins_code = C.VEX_VCVTPH2PS_YMM_XMMM128 if wide else C.VEX_VCVTPH2PS_XMM_XMMM64
            ins = I.create_reg_mem(ins_code, (YMM if wide else XMM)[dst], mem)
            return Case(kind, ins, gprs, ymms, flags, data, stack), ('fill', packed[:16 if wide else 8])
        src = rng.randrange(16)
        ymms[src][:16] = packed
        ins_code = C.VEX_VCVTPH2PS_YMM_XMMM128 if wide else C.VEX_VCVTPH2PS_XMM_XMMM64
        ins = I.create_reg_reg(ins_code, (YMM if wide else XMM)[dst], XMM[src])
    elif kind == 'cvtps2ph':
        wide = rng.random() < 0.5
        src = rng.randrange(16)
        ymms[src][:] = struct.pack('<8I', *(float_value(rng) for _ in range(8)))
        imm = rng.choice((0, 4))                 # round to nearest even, or MXCSR's (also nearest even)
        if use_mem:
            mem, _ = memory(16 if wide else 8)
            code = C.VEX_VCVTPS2PH_XMMM128_YMM_IMM8 if wide else C.VEX_VCVTPS2PH_XMMM64_XMM_IMM8
            ins = I.create_mem_reg_u32(code, mem, (YMM if wide else XMM)[src], imm)
        else:
            dst = rng.randrange(16)
            code = C.VEX_VCVTPS2PH_XMMM128_YMM_IMM8 if wide else C.VEX_VCVTPS2PH_XMMM64_XMM_IMM8
            ins = I.create_reg_reg_u32(code, XMM[dst], (YMM if wide else XMM)[src], imm)
    else:
        raise ValueError(kind)
    return Case(kind, ins, gprs, ymms, flags, data, stack), None


def state_of(machine, case, fill):
    block = bytearray(BLOCK_SIZE)
    for n, v in enumerate(case.gprs):
        struct.pack_into('<Q', block, GPRS + 8 * n, v & (1 << 64) - 1)
    struct.pack_into('<Q', block, FLAGS, case.flags)
    for n, y in enumerate(case.ymms):
        block[YMMS + 32 * n:YMMS + 32 * n + 32] = y
    struct.pack_into('<Q', block, GUEST, machine.guest_rsp)
    block[DATA:DATA + DATA_SIZE] = case.data
    block[STACK:STACK + STACK_SIZE] = case.stack
    return block, fill


def apply_fill(block, fill, machine, instruction):
    """For memory sources whose contents matter (halves), write them at the operand's address."""
    if fill is None:
        return
    _, packed = fill
    # the address the instruction reads: computed from the block's registers
    base, index = instruction.memory_base, instruction.memory_index
    if base == R.RIP:
        address = instruction.memory_displacement
    elif base == R.RSP:
        address = machine.guest_rsp + instruction.memory_displacement
    else:
        regs = {GPR64[n]: struct.unpack_from('<Q', block, GPRS + 8 * n)[0] for n in range(16)}
        disp = instruction.memory_displacement
        if disp >= 1 << 63:
            disp -= 1 << 64
        address = regs[base] + disp
        if index != R.NONE:
            address += regs[index] * instruction.memory_index_scale
        address &= (1 << 64) - 1
    offset = address - machine.block
    block[offset:offset + len(packed)] = packed


def compare(kind, real, emulated, guest_rsp_offset):
    """Differences between the two final blocks (empty when they match)."""
    problems = []
    for n in range(16):
        a, b = (struct.unpack_from('<Q', blk, GPRS + 8 * n)[0] for blk in (real, emulated))
        if a != b:
            problems.append(f'gpr{n}: real {a:#x} emulated {b:#x}')
    mask = DEFINED[kind]
    fa, fb = (struct.unpack_from('<Q', blk, FLAGS)[0] for blk in (real, emulated))
    if (fa ^ fb) & mask:
        problems.append(f'flags: real {fa:#x} emulated {fb:#x} (mask {mask:#x})')
    for n in range(16):
        a, b = (blk[YMMS + 32 * n:YMMS + 32 * n + 32] for blk in (real, emulated))
        if a != b:
            problems.append(f'ymm{n}: real {a.hex()} emulated {b.hex()}')
    if real[DATA:DATA + DATA_SIZE] != emulated[DATA:DATA + DATA_SIZE]:
        problems.append('data memory differs')
    low = STACK + guest_rsp_offset - isa_emulation.RED_ZONE      # the red zone and everything above
    if real[low:STACK + STACK_SIZE] != emulated[low:STACK + STACK_SIZE]:
        problems.append('stack (red zone or above) differs')
    return problems


@unittest.skipUnless(supported(), 'needs Windows x64, a Haswell-class CPU and iced_x86 in out/pydeps')
class IsaEmulationTests(unittest.TestCase):
    def test_against_real_instructions(self):
        machine = Machine()
        rng = random.Random(int(os.environ.get('TLOU_ISA_TEST_SEED', '1')))
        cases = int(os.environ.get('TLOU_ISA_TEST_CASES', '400'))
        failures = []
        for kind in DEFINED:
            for i in range(cases):
                case, fill = make_case(rng, machine, kind)
                block, fill = state_of(machine, case, fill)
                apply_fill(block, fill, machine, case.instruction)
                real = machine.run(lambda ip: _encode_one(case.instruction, ip), block)
                raw = _encode_one(case.instruction, machine.base)
                site = isa_emulation.parse(raw, machine.base)
                self.assertIsNotNone(site, f'{kind}: {case.instruction} not parsed')
                self.assertEqual(site.kind, kind)

                def body(ip):
                    raw_here = _encode_one(case.instruction, ip)
                    return isa_emulation.emulate(isa_emulation.parse(raw_here, ip), ip)
                emulated = machine.run(body, block)
                problems = compare(kind, real, emulated, GUEST_OFF)
                if problems:
                    failures.append(f'{kind} #{i} {case.instruction}: ' + '; '.join(problems[:4]))
        self.assertEqual(failures[:20], [], f'{len(failures)} mismatches')


def _encode_one(instruction, ip):
    encoder = Encoder(64)
    encoder.encode(instruction, ip)
    return bytes(encoder.take_buffer())


if __name__ == '__main__':
    unittest.main()
