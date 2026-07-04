//! Z80 -> Rust static recompiler (host tool).
//!
//! Reads a flat Z80 ROM image, decodes a contiguous code range, and EMITS a
//! `#![no_std]` Rust crate (gencrate/src/lib.rs) whose `run()` reproduces the
//! routine's semantics on a `Z80State` struct. That crate is then compiled to
//! native m68k by `rustc -Z build-std` (NO interpreter at runtime) and validated
//! on the pure-Rust m68k-rs core against the z80.c reference.
//!
//! Design (N64Recomp-style address-keyed dispatch):
//!   - One `Z80State { a..f: u8, ix,iy,sp,pc: u16, mem: *mut u8 }` (repr C).
//!   - The routine becomes `loop { match st.pc { 0xADDR => { <block> } ... } }`.
//!     Each Z80 instruction is one match arm keyed by its address; it mutates
//!     state and writes the *next* pc (fall-through OR branch target). Computed
//!     control flow (RET / RET cc / jumps that leave the recompiled range) lands
//!     on `_ => return`, i.e. the function boundary -- for the full ROM that arm
//!     would instead dispatch into other recompiled functions.
//!   - Flags are computed to match z80.c byte-for-byte (SZYX/SZYXP/OVERFLOW
//!     semantics from src/cores/tables.h), via tiny libcall-free helper fns.
//!
//! Usage:
//!   z80_recompiler <rom> <base_hex> <start_hex> <end_hex> <out_lib.rs>
//!   e.g. z80_recompiler games/1943/roms/1.12d 0 0x01da 0x01fe gencrate/src/lib.rs

use std::collections::BTreeSet;
use std::fs;

/// One decoded Z80 instruction: its address, byte length, the Rust body that
/// updates Z80State, and a human-readable disasm comment.
struct Insn {
    addr: u16,
    len: u16,
    body: String,
    asm: String,
    op: String, // opcode mnemonic family, for the coverage report
}

fn rd8(img: &[u8], a: u16) -> u8 {
    img[a as usize]
}
fn rd16(img: &[u8], a: u16) -> u16 {
    (img[a as usize] as u16) | ((img[a as usize + 1] as u16) << 8)
}

/// Condition-code test on the F register, as a Rust boolean expression.
fn cc_expr(cc: u8) -> &'static str {
    match cc {
        0 => "st.f & ZF == 0",  // NZ
        1 => "st.f & ZF != 0",  // Z
        2 => "st.f & CF == 0",  // NC
        3 => "st.f & CF != 0",  // C
        _ => unreachable!(),
    }
}
fn cc_name(cc: u8) -> &'static str {
    ["nz", "z", "nc", "c"][cc as usize]
}

/// Decode ONE instruction at `addr`. Returns the Insn or an error naming the
/// unsupported opcode (so coverage gaps are explicit, not silently wrong).
fn decode(img: &[u8], addr: u16) -> Result<Insn, String> {
    let op = rd8(img, addr);
    // 8-bit register select used by 0x80-0xBF ALU ops (low 3 bits).
    let reg_field = |r: u8| -> Option<&'static str> {
        match r {
            0 => Some("st.b"),
            1 => Some("st.c"),
            2 => Some("st.d"),
            3 => Some("st.e"),
            4 => Some("st.h"),
            5 => Some("st.l"),
            7 => Some("st.a"),
            _ => None, // 6 = (HL): not needed by this routine
        }
    };

    match op {
        // ---- LD A,(nn) : 0x3A nn nn ----
        0x3A => {
            let nn = rd16(img, addr + 1);
            Ok(Insn {
                addr,
                len: 3,
                body: format!("st.a = rd(st, 0x{nn:04x}); st.pc = 0x{:04x};", addr + 3),
                asm: format!("ld a,(${nn:04x})"),
                op: "LD A,(nn)".into(),
            })
        }
        // ---- LD A,n : 0x3E n ----
        0x3E => {
            let n = rd8(img, addr + 1);
            Ok(Insn {
                addr,
                len: 2,
                body: format!("st.a = 0x{n:02x}; st.pc = 0x{:04x};", addr + 2),
                asm: format!("ld a,${n:02x}"),
                op: "LD A,n".into(),
            })
        }
        // ---- AND r : 0xA0-0xA7 ----
        0xA0..=0xA7 => {
            let reg = reg_field(op & 7)
                .ok_or_else(|| format!("AND (HL) (0x{op:02x}) not yet supported @{addr:04x}"))?;
            Ok(Insn {
                addr,
                len: 1,
                body: format!(
                    "let (r, fl) = and8(st.a, {reg}); st.a = r; st.f = fl; st.pc = 0x{:04x};",
                    addr + 1
                ),
                asm: format!("and {}", ["b", "c", "d", "e", "h", "l", "(hl)", "a"][(op & 7) as usize]),
                op: "AND r".into(),
            })
        }
        // ---- CP n : 0xFE n ----
        0xFE => {
            let n = rd8(img, addr + 1);
            Ok(Insn {
                addr,
                len: 2,
                body: format!("st.f = cp_flags(st.a, 0x{n:02x}); st.pc = 0x{:04x};", addr + 2),
                asm: format!("cp ${n:02x}"),
                op: "CP n".into(),
            })
        }
        // ---- JR cc,e and JR e ----
        // 0x18 = JR e ; 0x20/0x28/0x30/0x38 = JR NZ/Z/NC/C,e
        0x18 | 0x20 | 0x28 | 0x30 | 0x38 => {
            let e = rd8(img, addr + 1) as i8;
            let target = (addr as i32 + 2 + e as i32) as u16;
            let fall = addr + 2;
            if op == 0x18 {
                Ok(Insn {
                    addr,
                    len: 2,
                    body: format!("st.pc = 0x{target:04x};"),
                    asm: format!("jr ${target:04x}"),
                    op: "JR e".into(),
                })
            } else {
                let cc = (op >> 3) & 3; // 0=NZ 1=Z 2=NC 3=C
                // BRANCHLESS select. A plain `if cond { pc=A } else { pc=B }`
                // makes LLVM's m68k backend schedule a flag-setting `move`
                // between the `cmpi` and the `beq`, which corrupts Z on real m68k
                // (MOVE updates CCR). `sel()` lowers to seq/and/eor with no
                // conditional branch, so there is no flag hazard.
                Ok(Insn {
                    addr,
                    len: 2,
                    body: format!(
                        "st.pc = sel({}, 0x{target:04x}, 0x{fall:04x});",
                        cc_expr(cc)
                    ),
                    asm: format!("jr {},${target:04x}", cc_name(cc)),
                    op: "JR cc,e".into(),
                })
            }
        }
        // ---- RET : 0xC9 ----
        0xC9 => Ok(Insn {
            addr,
            len: 1,
            body: "ret(st);".into(),
            asm: "ret".into(),
            op: "RET".into(),
        }),
        // ---- RET cc : 0xC0/0xC8/0xD0/0xD8 (NZ/Z/NC/C) ----
        0xC0 | 0xC8 | 0xD0 | 0xD8 => {
            let cc = (op >> 3) & 3;
            Ok(Insn {
                addr,
                len: 1,
                body: format!(
                    "if {} {{ ret(st); }} else {{ st.pc = 0x{:04x}; }}",
                    cc_expr(cc),
                    addr + 1
                ),
                asm: format!("ret {}", cc_name(cc)),
                op: "RET cc".into(),
            })
        }
        // ---- 0xDD prefix: IX-indexed ops ----
        0xDD => {
            let op2 = rd8(img, addr + 1);
            match op2 {
                // LD A,(IX+d) : 0xDD 0x7E d  (and generic LD r,(IX+d) 0x46..0x7E)
                0x46 | 0x4E | 0x56 | 0x5E | 0x66 | 0x6E | 0x7E => {
                    let d = rd8(img, addr + 2) as i8;
                    let dst = ["st.b", "st.c", "st.d", "st.e", "st.h", "st.l", "(hl)", "st.a"]
                        [((op2 >> 3) & 7) as usize];
                    if dst == "(hl)" {
                        return Err(format!("LD (HL),(IX+d)? 0xDD{op2:02x} @{addr:04x}"));
                    }
                    Ok(Insn {
                        addr,
                        len: 3,
                        body: format!(
                            "{dst} = rd(st, st.ix.wrapping_add({d}i16 as u16) as u32); st.pc = 0x{:04x};",
                            addr + 3
                        ),
                        asm: format!(
                            "ld {},(ix{:+})",
                            ["b", "c", "d", "e", "h", "l", "(hl)", "a"][((op2 >> 3) & 7) as usize],
                            d
                        ),
                        op: "LD r,(IX+d)".into(),
                    })
                }
                // INC (IX+d) : 0xDD 0x34 d
                0x34 => {
                    let d = rd8(img, addr + 2) as i8;
                    Ok(Insn {
                        addr,
                        len: 3,
                        body: format!(
                            "let ea = st.ix.wrapping_add({d}i16 as u16); \
                             let (r, fl) = inc8(st.f, rd(st, ea as u32)); wr(st, ea as u32, r); st.f = fl; \
                             st.pc = 0x{:04x};",
                            addr + 3
                        ),
                        asm: format!("inc (ix{d:+})"),
                        op: "INC (IX+d)".into(),
                    })
                }
                // LD (IX+d),n : 0xDD 0x36 d n
                0x36 => {
                    let d = rd8(img, addr + 2) as i8;
                    let n = rd8(img, addr + 3);
                    Ok(Insn {
                        addr,
                        len: 4,
                        body: format!(
                            "wr(st, st.ix.wrapping_add({d}i16 as u16) as u32, 0x{n:02x}); st.pc = 0x{:04x};",
                            addr + 4
                        ),
                        asm: format!("ld (ix{d:+}),${n:02x}"),
                        op: "LD (IX+d),n".into(),
                    })
                }
                _ => Err(format!("unsupported DD-prefixed opcode 0xDD 0x{op2:02x} @{addr:04x}")),
            }
        }
        _ => Err(format!("unsupported opcode 0x{op:02x} @{addr:04x}")),
    }
}

const PRELUDE: &str = r##"// GENERATED by tools/z80_recompiler -- DO NOT EDIT.
// A statically recompiled Z80 routine. Compiles to native m68k via:
//   cargo +nightly build -Z build-std=core --target m68k-unknown-none-elf --release
// The SAME source also builds for the host (cargo build --features host) so the
// recompiled logic can be diffed against z80.c on x86, isolating codegen issues.
#![cfg_attr(not(feature = "host"), no_std)]
#![allow(dead_code, clippy::all)]
#[cfg(not(feature = "host"))]
use core::panic::PanicInfo;
#[cfg(not(feature = "host"))]
#[panic_handler]
fn ph(_: &PanicInfo) -> ! { loop {} }

/// Z80 architectural state. repr(C), no padding for this field order on m68k
/// (u8*8 @0..8, u16*4 @8/10/12/14, *mut u8 @16; size 20, align 2). The harness
/// builds/reads these bytes big-endian (m68k) at a known address.
#[repr(C)]
pub struct Z80State {
    pub a: u8, pub b: u8, pub c: u8, pub d: u8,
    pub e: u8, pub h: u8, pub l: u8, pub f: u8,
    pub ix: u16, pub iy: u16, pub sp: u16, pub pc: u16,
    pub mem: *mut u8,
}

const ZF: u8 = 0x40; // Z flag
const CF: u8 = 0x01; // C flag

// NB: addresses are u32, NOT u16. A u16 address >= 0x8000 gets materialised by
// the LLVM m68k backend with `movea.w` (which SIGN-extends), so the indexed
// access wraps to the low 64K instead of `mem + addr`. Taking u32 forces a full
// 32-bit immediate / zero-extension and the effective address is correct (this
// also matches real m68k behaviour). Callers pass `addr as u32`.
// `black_box(a)` stops the LLVM m68k backend from hoisting a constant Z80
// address >= 0x8000 into an *address register* via sign-extending `movea.w`
// (which would corrupt `mem + addr` -- see the u32 note above). It keeps the
// address in a data register, materialised with a correct 32-bit immediate.
#[inline(always)]
fn rd(st: &Z80State, a: u32) -> u8 {
    let a = core::hint::black_box(a);
    unsafe { *st.mem.add(a as usize) }
}
#[inline(always)]
fn wr(st: &Z80State, a: u32, v: u8) {
    let a = core::hint::black_box(a);
    unsafe { *st.mem.add(a as usize) = v; }
}

// ---- flag helpers: byte-exact with src/cores/z80.c (tables.h) ----
#[inline(always)]
fn parity_even(b: u8) -> bool { let mut x = b; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1; (x & 1) == 0 }
// SZYX_FLAGS_TABLE[b] = (b & 0xA8) | (Z if b==0)
#[inline(always)]
fn szyx(b: u8) -> u8 { (b & 0xA8) | if b == 0 { 0x40 } else { 0 } }
// SZYXP_FLAGS_TABLE[b] = szyx(b) | (P if even parity)
#[inline(always)]
fn szyxp(b: u8) -> u8 { szyx(b) | if parity_even(b) { 0x04 } else { 0 } }
// OVERFLOW_TABLE[(c>>7)&3] = V iff carry-in(7) ^ carry-out(7)
#[inline(always)]
fn overflow(c: i32) -> i32 { match (c >> 7) & 3 { 1 | 2 => 0x04, _ => 0 } }

// AND x : F = SZYXP[A&x] | H ; returns (result, F)
#[inline(always)]
fn and8(a: u8, x: u8) -> (u8, u8) { let r = a & x; (r, szyxp(r) | 0x10) }

// CP x : flags of (A - x), A unchanged. Mirrors z80.c CP(x) exactly.
#[inline(always)]
fn cp_flags(a: u8, x: u8) -> u8 {
    let (ai, xi) = (a as i32, x as i32);
    let z = ai - xi;
    let mut c = ai ^ xi ^ z;
    let mut f = 0x02 | (c & 0x10);                 // N | (H from a^x^z)
    f |= (szyx((z & 0xff) as u8) as i32) & 0xC0;    // S,Z from result
    f |= xi & 0x28;                                  // Y,X from operand (Z80 quirk)
    c &= 0x180;
    f |= overflow(c);                                // V
    f |= c >> 8;                                     // C
    f as u8
}

// INC x (memory/reg) : C preserved, N=0. Mirrors z80.c INC(x). returns (result,F).
#[inline(always)]
fn inc8(oldf: u8, x: u8) -> (u8, u8) {
    let xi = x as i32;
    let z = xi + 1;
    let c = xi ^ z;
    let mut f = (oldf as i32) & 0x01;                // keep C
    f |= c & 0x10;                                    // H
    f |= szyx((z & 0xff) as u8) as i32;               // S,Z,Y,X
    f |= overflow(c);                                 // V
    ((z & 0xff) as u8, f as u8)
}

// Branchless u16 select: `cond ? a : b` with NO conditional branch (see JR cc).
// `black_box` on the mask stops LLVM from re-recognising this as a select and
// lowering it back to a `beq` (which would reintroduce the move-before-branch
// CCR hazard on the m68k backend).
#[inline(always)]
fn sel(cond: bool, a: u16, b: u16) -> u16 {
    let m = core::hint::black_box((cond as u16).wrapping_neg()); // 0xffff if true, else 0
    b ^ (m & (a ^ b))
}

// RET : pop pc from the Z80 stack. Leaving the recompiled range hits `_ => return`.
#[inline(always)]
fn ret(st: &mut Z80State) {
    let lo = rd(st, st.sp as u32) as u16;
    let hi = rd(st, st.sp.wrapping_add(1) as u32) as u16;
    st.sp = st.sp.wrapping_add(2);
    st.pc = lo | (hi << 8);
}
"##;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 6 {
        eprintln!("usage: z80_recompiler <rom> <base_hex> <start_hex> <end_hex> <out_lib.rs>");
        std::process::exit(2);
    }
    let rom = fs::read(&args[1]).expect("read rom");
    let parse = |s: &str| u32::from_str_radix(s.trim_start_matches("0x"), 16).expect("hex");
    let base = parse(&args[2]);
    let start = parse(&args[3]) as u16;
    let end = parse(&args[4]) as u16;
    let out = &args[5];

    // Build a 64KB image with the ROM at `base`.
    let mut img = vec![0u8; 0x10000];
    let b = base as usize;
    img[b..b + rom.len()].copy_from_slice(&rom);

    // Linear recursive decode of the contiguous code range [start, end).
    let mut insns: Vec<Insn> = Vec::new();
    let mut ops_seen: BTreeSet<String> = BTreeSet::new();
    let mut addr = start;
    while addr < end {
        match decode(&img, addr) {
            Ok(i) => {
                ops_seen.insert(i.op.clone());
                addr += i.len;
                insns.push(i);
            }
            Err(e) => {
                eprintln!("DECODE ERROR: {e}");
                std::process::exit(1);
            }
        }
    }

    // Emit the crate.
    let mut s = String::new();
    s.push_str(PRELUDE);
    s.push_str("\n/// Statically recompiled routine ");
    s.push_str(&format!("[0x{start:04x}, 0x{end:04x}).\n"));
    s.push_str("#[no_mangle]\npub extern \"C\" fn run(state: *mut Z80State) {\n");
    s.push_str("    let st = unsafe { &mut *state };\n");
    s.push_str("    loop {\n        match st.pc {\n");
    for i in &insns {
        s.push_str(&format!(
            "            0x{:04x} => {{ {} }} // {}\n",
            i.addr, i.body, i.asm
        ));
    }
    s.push_str("            _ => return, // returned to caller / left recompiled range\n");
    s.push_str("        }\n    }\n}\n");

    fs::write(out, &s).expect("write lib.rs");

    // Report.
    eprintln!("recompiled {} instructions, [0x{start:04x},0x{end:04x}) -> {out}", insns.len());
    eprintln!("opcode families covered ({}):", ops_seen.len());
    for o in &ops_seen {
        eprintln!("  - {o}");
    }
}
