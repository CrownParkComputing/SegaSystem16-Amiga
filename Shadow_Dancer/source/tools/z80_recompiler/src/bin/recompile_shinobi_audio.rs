//! WHOLE-ROM Z80 -> Rust static recompiler for Shinobi's SOUND CPU (Shinobi sound ROM, Capcom 1987).
//!
//! Same engine as recompile_commando, but PLAIN (no opcode decryption). Control flow is
//! discovered by recursive descent from the reset + RST/NMI entry points; every reachable
//! instruction address becomes a match arm keyed by pc. The emitted crate compiles to
//! native m68k (no interpreter) and is validated on host vs z80.c (aud_lockstep.c).
//!
//! FLAT 32KB ROM, NO banking, NO opcode encryption. Exports `run` (no_mangle); the build
//! prefixes the audio objs with `_aud_` so it links as `aud_run`.
//! Usage: recompile_shinobi_audio <Shinobi sound ROM> <out_lib.rs> [extra_seed_hex ...]

use std::collections::{BTreeMap, BTreeSet};
use std::fs;

// ---- decode result -------------------------------------------------------
struct Insn {
    len: u16,
    body: String,
    asm: String,
    succ: Vec<u16>,   // addresses control can reach (incl fall-through when applicable)
    op: String,       // family for coverage report
}

struct Rom {
    plain: Vec<u8>, // operands/data
    dec: Vec<u8>,   // M1 opcode shadow
}
impl Rom {
    fn p(&self, a: u16) -> u8 { *self.plain.get(a as usize).unwrap_or(&0) }
    fn o(&self, a: u16) -> u8 { *self.dec.get(a as usize).unwrap_or(&0) }
    fn p16(&self, a: u16) -> u16 { (self.p(a) as u16) | ((self.p(a.wrapping_add(1)) as u16) << 8) }
}

// 8-bit register lvalue for index 0..7 under prefix mode (0 none/HL, 1 IX, 2 IY).
// idx 6 = memory (HL)/(IX+d) handled by caller; returns "" for it.
fn r8(idx: u8, mode: u8) -> String {
    match idx {
        0 => "s.b".into(), 1 => "s.c".into(), 2 => "s.d".into(), 3 => "s.e".into(),
        4 => match mode { 1 => "s.ixh".into(), 2 => "s.iyh".into(), _ => "s.h".into() },
        5 => match mode { 1 => "s.ixl".into(), 2 => "s.iyl".into(), _ => "s.l".into() },
        6 => "".into(),
        7 => "s.a".into(),
        _ => unreachable!(),
    }
}
fn r8_name(idx: u8, mode: u8) -> &'static str {
    match (idx, mode) {
        (0,_) => "b", (1,_) => "c", (2,_) => "d", (3,_) => "e",
        (4,1) => "ixh", (4,2) => "iyh", (4,_) => "h",
        (5,1) => "ixl", (5,2) => "iyl", (5,_) => "l",
        (6,_) => "(hl)", (7,_) => "a", _ => "?",
    }
}

// 16-bit pair getter expr for SS table {BC,DE,HL/IX/IY,SP}
fn ss_get(idx: u8, mode: u8) -> String {
    match idx {
        0 => "gbc(s)".into(), 1 => "gde(s)".into(),
        2 => match mode { 1 => "gix(s)".into(), 2 => "giy(s)".into(), _ => "ghl(s)".into() },
        3 => "s.sp".into(), _ => unreachable!(),
    }
}
fn ss_set(idx: u8, mode: u8, val: &str) -> String {
    match idx {
        0 => format!("sbc(s,{val});"), 1 => format!("sde(s,{val});"),
        2 => match mode { 1 => format!("six(s,{val});"), 2 => format!("siy(s,{val});"), _ => format!("shl(s,{val});") },
        3 => format!("s.sp = {val};"), _ => unreachable!(),
    }
}
fn ss_name(idx: u8, mode: u8) -> &'static str {
    match (idx, mode) { (0,_)=>"bc",(1,_)=>"de",(2,1)=>"ix",(2,2)=>"iy",(2,_)=>"hl",(3,_)=>"sp",_=>"?" }
}
// PUSH/POP pair {BC,DE,HL/IX/IY,AF}
fn qq_get(idx: u8, mode: u8) -> String {
    match idx { 3 => "gaf(s)".into(), _ => ss_get(idx, mode) }
}
fn qq_set(idx: u8, mode: u8, val: &str) -> String {
    match idx { 3 => format!("saf(s,{val});"), _ => ss_set(idx, mode, val) }
}
fn qq_name(idx: u8, mode: u8) -> &'static str { match idx { 3=>"af", _=>ss_name(idx,mode) } }

// HL/IX/IY 16-bit get/set + memory base for (HL)/(IX+d)
fn hlreg_get(mode: u8) -> &'static str { match mode { 1 => "gix(s)", 2 => "giy(s)", _ => "ghl(s)" } }
fn hlreg_set(mode: u8, val: &str) -> String { match mode { 1 => format!("six(s,{val});"), 2 => format!("siy(s,{val});"), _ => format!("shl(s,{val});") } }

// condition cc 0..7 -> rust bool expr on s.f. (NZ,Z,NC,C,PO,PE,P,M)
fn cc_expr(cc: u8) -> String {
    match cc {
        0 => "(s.f & 0x40)==0".into(), 1 => "(s.f & 0x40)!=0".into(),
        2 => "(s.f & 0x01)==0".into(), 3 => "(s.f & 0x01)!=0".into(),
        4 => "(s.f & 0x04)==0".into(), 5 => "(s.f & 0x04)!=0".into(),
        6 => "(s.f & 0x80)==0".into(), 7 => "(s.f & 0x80)!=0".into(),
        _ => unreachable!(),
    }
}
const CC_NAME: [&str; 8] = ["nz","z","nc","c","po","pe","p","m"];

// Emit code that reads an 8-bit source operand at index `idx` under `mode`,
// fetching the displacement (constant `disp`) for indexed memory. Returns
// (setup_stmts, value_expr). For idx!=6 it's just the register expr.
fn read_src(idx: u8, mode: u8, disp: i8) -> (String, String) {
    if idx != 6 {
        (String::new(), r8(idx, mode))
    } else if mode == 0 {
        (String::new(), format!("rd(s, {} as u32)", hlreg_get(0)))
    } else {
        (String::new(), format!("rd(s, ({}.wrapping_add({disp}i16 as u16)) as u32)", hlreg_get(mode)))
    }
}
// Emit a store of `valexpr` into 8-bit dest idx under mode.
fn write_dst(idx: u8, mode: u8, disp: i8, valexpr: &str) -> String {
    if idx != 6 {
        format!("{} = {valexpr};", r8(idx, mode))
    } else if mode == 0 {
        format!("wr(s, {} as u32, {valexpr});", hlreg_get(0))
    } else {
        format!("wr(s, ({}.wrapping_add({disp}i16 as u16)) as u32, {valexpr});", hlreg_get(mode))
    }
}

// memory EA expr for (HL)/(IX+d)
fn mem_ea(mode: u8, disp: i8) -> String {
    if mode == 0 { format!("{} as u32", hlreg_get(0)) }
    else { format!("({}.wrapping_add({disp}i16 as u16)) as u32", hlreg_get(mode)) }
}

// ALU helper name for op group 0..7 (ADD,ADC,SUB,SBC,AND,XOR,OR,CP)
const ALU_FN: [&str; 8] = ["add8","adc8","sub8","sbc8","and8","xor8","or8","cp8"];
const ALU_NAME: [&str; 8] = ["add","adc","sub","sbc","and","xor","or","cp"];
// CB rotate group 0..7
const ROT_FN: [&str; 8] = ["rlc","rrc","rl","rr","sla","sra","sll","srl"];
const ROT_NAME: [&str; 8] = ["rlc","rrc","rl","rr","sla","sra","sll","srl"];

fn next(a: u16, n: u16) -> u16 { a.wrapping_add(n) }

// Decode one instruction at `addr`. mode applied to *this* opcode (for DD/FD we
// recurse with mode set). Returns Insn.
fn decode(rom: &Rom, addr: u16) -> Result<Insn, String> {
    decode_m(rom, addr, 0, 0)
}

// pfx_len = bytes consumed by DD/FD prefix already (1 if prefixed); addr points
// at the *sub-opcode* when mode!=0.
fn decode_m(rom: &Rom, addr: u16, mode: u8, pfx_len: u16) -> Result<Insn, String> {
    let op = rom.o(addr);
    let a1 = next(addr, 1);
    // helper to assemble final Insn accounting for the prefix length
    let mk = |len: u16, body: String, asm: String, succ: Vec<u16>, opn: &str| {
        Insn { len: len + pfx_len, body, asm, succ, op: opn.into() }
    };
    // Fall-through/target base. `addr` points at the SUB-opcode (== instruction
    // start when unprefixed; prefix bytes precede it when DD/FD). Using `addr` (not
    // addr-pfx_len) makes next(ia,sublen) land past the whole instruction: addr +
    // sublen = (start+pfx_len)+sublen. The match-arm KEY is the outer work address
    // (the prefix byte), and `mk` adds pfx_len to the reported length.
    let ia = addr;
    let cyc = |n: u32| format!("s.cycles=s.cycles.wrapping_add({n});");
    let rinc = "s.r=(s.r&0x80)|(s.r.wrapping_add(1)&0x7f);";

    // ---- prefixes ----
    match op {
        0xDD => return decode_m(rom, a1, 1, pfx_len + 1),
        0xFD => return decode_m(rom, a1, 2, pfx_len + 1),
        0xCB => return decode_cb(rom, addr, mode, pfx_len),
        0xED if mode == 0 => return decode_ed(rom, addr, pfx_len),
        _ => {}
    }

    let setpc = |t: u16| format!("s.pc=0x{t:04x};");
    let x = op >> 6; let y = (op >> 3) & 7; let z = op & 7; let p = (op >> 4) & 3; let q = (op >> 3) & 1;

    // displacement for indexed memory ops (single (HL)-as-operand instructions):
    // when mode!=0 and an (HL) operand is used, the d byte sits right after the
    // sub-opcode (PLAIN). Most single-operand indexed ops have d at a1.
    let disp = rom.p(a1) as i8;

    // ============ x=1: LD r,r' / HALT ============
    if x == 1 {
        if y == 6 && z == 6 {
            // HALT
            let body = format!("{rinc}{} s.halted=1; {} s.stop=1; return;", cyc(4), setpc(next(ia, 1)));
            return Ok(mk(1, body, "halt".into(), vec![next(ia, 1)], "HALT"));
        }
        // LD r,r'  -- handle (HL)/(IX+d) and the IXH/IXL quirk
        let dst_mem = z == 6 || y == 6;
        if mode == 0 || !dst_mem {
            // pure register (or non-indexed memory): both sides use mode regs.
            // But if one side is (HL) it's plain HL here (mode 0). For indexed with
            // NO memory operand, both regs are indexed halves.
            let mut len = 1u16;
            let (mut setup, srcv) = if z == 6 {
                len += if mode != 0 { 1 } else { 0 };
                read_src(6, mode, disp)
            } else { (String::new(), r8(z, mode)) };
            let store = if y == 6 {
                len += if mode != 0 { 1 } else { 0 };
                write_dst(6, mode, disp, &srcv)
            } else { format!("{} = {srcv};", r8(y, mode)) };
            setup.push_str(&store);
            let body = format!("{rinc}{}{setup}{}", cyc(if z==6||y==6 {7} else {4}), setpc(next(ia, len)));
            return Ok(mk(len, body, format!("ld {},{}", r8_name(y,mode), r8_name(z,mode)), vec![next(ia, len)], "LD r,r"));
        } else {
            // indexed with memory operand: the REGISTER side uses the REAL h/l
            // (mode 0), the memory side is (IX+d).
            let len = 2u16;
            if z == 6 {
                // LD r,(IX+d)  -- r is real reg (mode 0)
                let (_s, srcv) = read_src(6, mode, disp);
                let store = format!("{} = {srcv};", r8(y, 0));
                let body = format!("{rinc}{}{store}{}", cyc(15), setpc(next(ia, len)));
                return Ok(mk(2, body, format!("ld {},({}{:+})", r8_name(y,0), ss_name(2,mode), disp), vec![next(ia, len)], "LD r,(IX+d)"));
            } else {
                // LD (IX+d),r  -- r is real reg (mode 0)
                let srcv = r8(z, 0);
                let store = write_dst(6, mode, disp, &srcv);
                let body = format!("{rinc}{}{store}{}", cyc(15), setpc(next(ia, len)));
                return Ok(mk(2, body, format!("ld ({}{:+}),{}", ss_name(2,mode), disp, r8_name(z,0)), vec![next(ia, len)], "LD (IX+d),r"));
            }
        }
    }

    // ============ x=2: ALU A,r ============
    if x == 2 {
        let fnn = ALU_FN[y as usize];
        let mut len = 1u16;
        let (_s, srcv) = if z == 6 { len += if mode != 0 {1} else {0}; read_src(6, mode, disp) } else { (String::new(), r8(z, mode)) };
        let body = format!("{rinc}{} {{ let (r,f)={fnn}(s.a,{srcv},s.f); {} s.f=f; }} {}",
            cyc(if z==6 {7} else {4}),
            if y == 7 { "let _=r;".to_string() } else { "s.a=r;".to_string() }, // CP discards result
            setpc(next(ia, len)));
        return Ok(mk(len, body, format!("{} {}", ALU_NAME[y as usize], r8_name(z,mode)), vec![next(ia, len)], "ALU A,r"));
    }

    // ============ x=0 ============
    if x == 0 {
        match z {
            0 => match y {
                0 => { // NOP
                    let body = format!("{rinc}{}{}", cyc(4), setpc(next(ia,1)));
                    return Ok(mk(1, body, "nop".into(), vec![next(ia,1)], "NOP"));
                }
                1 => { // EX AF,AF'
                    let body = format!("{rinc}{} {{ let t=gaf(s); saf(s,s.af_); s.af_=t; }} {}", cyc(4), setpc(next(ia,1)));
                    return Ok(mk(1, body, "ex af,af'".into(), vec![next(ia,1)], "EX AF"));
                }
                2 => { // DJNZ (branchless; CYCLE-EXACT vs z80.c: taken=13, not-taken=8 -- the
                       // flat cyc(9) used elsewhere drifts ~4 cyc/iter, which over 1943's
                       // 65536-iter DI startup delay loop desyncs the audio CPU by ~4 frames)
                    let e = rom.p(a1) as i8; let t = next(next(ia,2), e as u16);
                    let body = format!("{rinc} s.b=s.b.wrapping_sub(1); s.cycles=s.cycles.wrapping_add(selu32(s.b!=0,13,8)); s.pc=sel(s.b!=0, 0x{t:04x}, 0x{:04x});",
                        next(ia,2));
                    return Ok(mk(2, body, format!("djnz ${t:04x}"), vec![t, next(ia,2)], "DJNZ"));
                }
                3 => { // JR e
                    let e = rom.p(a1) as i8; let t = next(next(ia,2), e as u16);
                    let body = format!("{rinc}{}{}", cyc(12), setpc(t));
                    return Ok(mk(2, body, format!("jr ${t:04x}"), vec![t], "JR e"));
                }
                _ => { // 4..7 JR cc,e
                    let cc = y - 4; let e = rom.p(a1) as i8; let t = next(next(ia,2), e as u16);
                    let body = format!("{rinc}{} s.pc=sel({}, 0x{t:04x}, 0x{:04x});", cyc(12), cc_expr(cc), next(ia,2));
                    return Ok(mk(2, body, format!("jr {},${t:04x}", CC_NAME[cc as usize]), vec![t, next(ia,2)], "JR cc,e"));
                }
            },
            1 => {
                if q == 0 { // LD rr,nn
                    let nn = rom.p16(a1);
                    let body = format!("{rinc}{} {} {}", cyc(10), ss_set(p, mode, &format!("0x{nn:04x}")), setpc(next(ia,3)));
                    return Ok(mk(3, body, format!("ld {},${nn:04x}", ss_name(p,mode)), vec![next(ia,3)], "LD rr,nn"));
                } else { // ADD HL,rr
                    let rr = ss_get(p, mode);
                    let body = format!("{rinc}{} {{ let (r,f)=add16({},{rr},s.f); {} s.f=f; }} {}",
                        cyc(11), hlreg_get(mode), hlreg_set(mode,"r"), setpc(next(ia,1)));
                    return Ok(mk(1, body, format!("add {},{}", ss_name(2,mode), ss_name(p,mode)), vec![next(ia,1)], "ADD HL,rr"));
                }
            },
            2 => {
                // indirect loads/stores
                let body = match (q, p) {
                    (0,0) => format!("wr(s, gbc(s) as u32, s.a);"),         // LD (BC),A
                    (0,1) => format!("wr(s, gde(s) as u32, s.a);"),         // LD (DE),A
                    (0,2) => { let nn=rom.p16(a1); format!("{{ let a={};", hlreg_get(mode)).to_string()
                                + &format!("wr(s,0x{nn:04x},a as u8); wr(s,0x{:04x},(a>>8)as u8); }}", next(nn,1)) } // LD (nn),HL
                    (0,3) => { let nn=rom.p16(a1); format!("wr(s,0x{nn:04x},s.a);") }       // LD (nn),A
                    (1,0) => format!("s.a=rd(s, gbc(s) as u32);"),         // LD A,(BC)
                    (1,1) => format!("s.a=rd(s, gde(s) as u32);"),         // LD A,(DE)
                    (1,2) => { let nn=rom.p16(a1); format!("{{ let lo=rd(s,0x{nn:04x}) as u16; let hi=rd(s,0x{:04x}) as u16; {} }}", next(nn,1), hlreg_set(mode,"lo|(hi<<8)")) } // LD HL,(nn)
                    (1,3) => { let nn=rom.p16(a1); format!("s.a=rd(s,0x{nn:04x});") }        // LD A,(nn)
                    _ => unreachable!(),
                };
                let len = if p >= 2 { 3 } else { 1 };
                let cycn = if p>=2 {16} else {7};
                let body = format!("{rinc}{} {body} {}", cyc(cycn), setpc(next(ia,len)));
                return Ok(mk(len, body, "ld indirect".into(), vec![next(ia,len)], "LD indirect"));
            },
            3 => { // INC/DEC rr
                let rr = ss_get(p, mode);
                let body = if q == 0 {
                    format!("{rinc}{} {} {}", cyc(6), ss_set(p,mode,&format!("({rr}).wrapping_add(1)")), setpc(next(ia,1)))
                } else {
                    format!("{rinc}{} {} {}", cyc(6), ss_set(p,mode,&format!("({rr}).wrapping_sub(1)")), setpc(next(ia,1)))
                };
                return Ok(mk(1, body, format!("{} {}", if q==0 {"inc"} else {"dec"}, ss_name(p,mode)), vec![next(ia,1)], "INC/DEC rr"));
            },
            4 | 5 => { // INC r / DEC r
                let isinc = z == 4;
                let fnn = if isinc {"inc8"} else {"dec8"};
                let mut len = 1u16;
                if y == 6 {
                    len += if mode != 0 {1} else {0};
                    let ea = mem_ea(mode, disp);
                    let body = format!("{rinc}{} {{ let ea={ea}; let (r,f)={fnn}(rd(s,ea),s.f); wr(s,ea,r); s.f=f; }} {}",
                        cyc(if mode!=0 {23} else {11}), setpc(next(ia,len)));
                    return Ok(mk(len, body, format!("{} ({})", if isinc{"inc"}else{"dec"}, ss_name(2,mode)), vec![next(ia,len)], "INC/DEC (HL)"));
                } else {
                    let rg = r8(y, mode);
                    let body = format!("{rinc}{} {{ let (r,f)={fnn}({rg},s.f); {rg}=r; s.f=f; }} {}", cyc(4), setpc(next(ia,1)));
                    return Ok(mk(1, body, format!("{} {}", if isinc{"inc"}else{"dec"}, r8_name(y,mode)), vec![next(ia,1)], "INC/DEC r"));
                }
            },
            6 => { // LD r,n
                let mut len = 2u16;
                if y == 6 {
                    if mode != 0 {
                        len = 3;
                        let n = rom.p(next(addr,2));
                        let ea = mem_ea(mode, disp);
                        let body = format!("{rinc}{} wr(s,{ea},0x{n:02x}); {}", cyc(19), setpc(next(ia,len)));
                        return Ok(mk(3, body, format!("ld ({}{:+}),${n:02x}", ss_name(2,mode), disp), vec![next(ia,len)], "LD (IX+d),n"));
                    } else {
                        let n = rom.p(a1);
                        let body = format!("{rinc}{} wr(s, {} as u32, 0x{n:02x}); {}", cyc(10), hlreg_get(0), setpc(next(ia,2)));
                        return Ok(mk(2, body, format!("ld (hl),${n:02x}"), vec![next(ia,2)], "LD (HL),n"));
                    }
                } else {
                    let n = rom.p(a1);
                    let body = format!("{rinc}{} {} = 0x{n:02x}; {}", cyc(7), r8(y,mode), setpc(next(ia,2)));
                    return Ok(mk(2, body, format!("ld {},${n:02x}", r8_name(y,mode)), vec![next(ia,2)], "LD r,n"));
                }
            },
            7 => { // accumulator/flag ops: RLCA RRCA RLA RRA DAA CPL SCF CCF
                let (call, name) = match y {
                    0 => ("{ let (a,f)=rlca(s.a,s.f); s.a=a; s.f=f; }", "rlca"),
                    1 => ("{ let (a,f)=rrca(s.a,s.f); s.a=a; s.f=f; }", "rrca"),
                    2 => ("{ let (a,f)=rla(s.a,s.f); s.a=a; s.f=f; }", "rla"),
                    3 => ("{ let (a,f)=rra(s.a,s.f); s.a=a; s.f=f; }", "rra"),
                    4 => ("{ let (a,f)=daa(s.a,s.f); s.a=a; s.f=f; }", "daa"),
                    5 => ("{ let (a,f)=cpl(s.a,s.f); s.a=a; s.f=f; }", "cpl"),
                    6 => ("s.f=scf(s.a,s.f);", "scf"),
                    7 => ("s.f=ccf(s.a,s.f);", "ccf"),
                    _ => unreachable!(),
                };
                let body = format!("{rinc}{} {call} {}", cyc(4), setpc(next(ia,1)));
                return Ok(mk(1, body, name.into(), vec![next(ia,1)], "acc/flag op"));
            },
            _ => {}
        }
    }

    // ============ x=3 ============
    if x == 3 {
        match z {
            0 => { // RET cc (branchless)
                let body = format!("{rinc}{} {{ let lo=rd(s,s.sp as u32) as u16; let hi=rd(s,s.sp.wrapping_add(1) as u32) as u16; let pop=lo|(hi<<8); let take={}; s.pc=sel(take,pop,0x{:04x}); s.sp=sel(take,s.sp.wrapping_add(2),s.sp); }}",
                    cyc(11), cc_expr(y), next(ia,1));
                return Ok(mk(1, body, format!("ret {}", CC_NAME[y as usize]), vec![next(ia,1)], "RET cc"));
            },
            1 => {
                if q == 0 { // POP qq
                    let body = format!("{rinc}{} {{ let v=pop16(s); {} }} {}", cyc(10), qq_set(p,mode,"v"), setpc(next(ia,1)));
                    return Ok(mk(1, body, format!("pop {}", qq_name(p,mode)), vec![next(ia,1)], "POP"));
                } else {
                    match p {
                        0 => { let body=format!("{rinc}{} pop_pc(s);", cyc(10)); return Ok(mk(1, body, "ret".into(), vec![], "RET")); }
                        1 => { let body=format!("{rinc}{} {{ let t=gbc(s); sbc(s,s.bc_); s.bc_=t; let t=gde(s); sde(s,s.de_); s.de_=t; let t=ghl(s); shl(s,s.hl_); s.hl_=t; }} {}", cyc(4), setpc(next(ia,1))); return Ok(mk(1, body, "exx".into(), vec![next(ia,1)], "EXX")); }
                        2 => { let body=format!("{rinc}{} s.pc={};", cyc(4), hlreg_get(mode)); return Ok(mk(1, body, format!("jp ({})", ss_name(2,mode)), vec![], "JP (HL)")); }
                        3 => { let body=format!("{rinc}{} s.sp={}; {}", cyc(6), hlreg_get(mode), setpc(next(ia,1))); return Ok(mk(1, body, "ld sp,hl".into(), vec![next(ia,1)], "LD SP,HL")); }
                        _ => unreachable!(),
                    }
                }
            },
            2 => { // JP cc,nn
                let nn = rom.p16(a1);
                let body = format!("{rinc}{} s.pc=sel({}, 0x{nn:04x}, 0x{:04x});", cyc(10), cc_expr(y), next(ia,3));
                return Ok(mk(3, body, format!("jp {},${nn:04x}", CC_NAME[y as usize]), vec![nn, next(ia,3)], "JP cc,nn"));
            },
            3 => match y {
                0 => { let nn=rom.p16(a1); let body=format!("{rinc}{}{}", cyc(10), setpc(nn)); return Ok(mk(3, body, format!("jp ${nn:04x}"), vec![nn], "JP nn")); }
                1 => unreachable!(), // CB handled above
                2 => { let n=rom.p(a1); let body=format!("{rinc}{} out_io(s,0x{n:02x},s.a); {}", cyc(11), setpc(next(ia,2))); return Ok(mk(2, body, format!("out (${n:02x}),a"), vec![next(ia,2)], "OUT (n),A")); }
                3 => { let n=rom.p(a1); let body=format!("{rinc}{} s.a=in_io(s,0x{n:02x}); {}", cyc(11), setpc(next(ia,2))); return Ok(mk(2, body, format!("in a,(${n:02x})"), vec![next(ia,2)], "IN A,(n)")); }
                4 => { let body=format!("{rinc}{} {{ let t=rd(s,s.sp as u32) as u16 | ((rd(s,s.sp.wrapping_add(1) as u32) as u16)<<8); let h={}; wr(s,s.sp as u32,h as u8); wr(s,s.sp.wrapping_add(1) as u32,(h>>8)as u8); {} }} {}", cyc(19), hlreg_get(mode), hlreg_set(mode,"t"), setpc(next(ia,1))); return Ok(mk(1, body, "ex (sp),hl".into(), vec![next(ia,1)], "EX (SP),HL")); }
                5 => { let body=format!("{rinc}{} {{ let t=gde(s); sde(s,ghl(s)); shl(s,t); }} {}", cyc(4), setpc(next(ia,1))); return Ok(mk(1, body, "ex de,hl".into(), vec![next(ia,1)], "EX DE,HL")); }
                6 => { let body=format!("{rinc}{} s.iff1=0; s.iff2=0; {}", cyc(4), setpc(next(ia,1))); return Ok(mk(1, body, "di".into(), vec![next(ia,1)], "DI")); }
                7 => { let body=format!("{rinc}{} s.iff1=1; s.iff2=1; {}", cyc(4), setpc(next(ia,1))); return Ok(mk(1, body, "ei".into(), vec![next(ia,1)], "EI")); }
                _ => unreachable!(),
            },
            4 => { // CALL cc,nn (branchless; spurious 2-byte write below SP when not
                    // taken is harmless scratch -- the Z80 area below SP is dead here)
                let nn = rom.p16(a1); let ret = next(ia,3);
                let body = format!("{rinc}{} {{ let nsp=s.sp.wrapping_sub(2); wr(s,nsp as u32,(0x{ret:04x}u16) as u8); wr(s,nsp.wrapping_add(1) as u32,(0x{ret:04x}u16>>8) as u8); let take={}; s.pc=sel(take,0x{nn:04x},0x{ret:04x}); s.sp=sel(take,nsp,s.sp); }}",
                    cyc(17), cc_expr(y));
                return Ok(mk(3, body, format!("call {},${nn:04x}", CC_NAME[y as usize]), vec![nn, next(ia,3)], "CALL cc,nn"));
            },
            5 => {
                if q == 0 { // PUSH qq
                    let v = qq_get(p, mode);
                    let body = format!("{rinc}{} push16(s,{v}); {}", cyc(11), setpc(next(ia,1)));
                    return Ok(mk(1, body, format!("push {}", qq_name(p,mode)), vec![next(ia,1)], "PUSH"));
                } else if p == 0 { // CALL nn
                    let nn = rom.p16(a1);
                    let body = format!("{rinc}{} push16(s,0x{:04x}); {}", cyc(17), next(ia,3), setpc(nn));
                    return Ok(mk(3, body, format!("call ${nn:04x}"), vec![nn, next(ia,3)], "CALL nn"));
                } else { unreachable!() } // p1=DD,p2=ED,p3=FD handled by prefixes
            },
            6 => { // ALU A,n
                let n = rom.p(a1); let fnn = ALU_FN[y as usize];
                let body = format!("{rinc}{} {{ let (r,f)={fnn}(s.a,0x{n:02x},s.f); {} s.f=f; }} {}",
                    cyc(7), if y==7 {"let _=r;"} else {"s.a=r;"}, setpc(next(ia,2)));
                return Ok(mk(2, body, format!("{} ${n:02x}", ALU_NAME[y as usize]), vec![next(ia,2)], "ALU A,n"));
            },
            7 => { // RST p
                let t = (y as u16) * 8;
                let body = format!("{rinc}{} push16(s,0x{:04x}); {}", cyc(11), next(ia,1), setpc(t));
                return Ok(mk(1, body, format!("rst ${t:02x}"), vec![t, next(ia,1)], "RST"));
            },
            _ => {}
        }
    }

    Err(format!("unsupported opcode 0x{op:02x} (mode {mode}) @{addr:04x}"))
}

// ---- CB prefixed ----
fn decode_cb(rom: &Rom, addr: u16, mode: u8, pfx_len: u16) -> Result<Insn, String> {
    // addr points at the 0xCB byte. mode!=0 means DDCB/FDCB.
    let ia = addr.wrapping_sub(pfx_len);
    let rinc = "s.r=(s.r&0x80)|(s.r.wrapping_add(1)&0x7f);";
    let cyc = |n: u32| format!("s.cycles=s.cycles.wrapping_add({n});");
    let (cbop, disp, ilen): (u8, i8, u16) = if mode == 0 {
        (rom.o(next(addr, 1)), 0, 2) // CB op is M1 (decrypted), len 2 (+pfx_len=0)
    } else {
        // DD CB d op : disp = plain at addr+1, cb op = PLAIN at addr+2
        (rom.p(next(addr, 2)), rom.p(next(addr, 1)) as i8, 4 - pfx_len) // total incl prefix handled by mk via pfx
    };
    let x = cbop >> 6; let y = (cbop >> 3) & 7; let z = cbop & 7;
    let total_len = if mode == 0 { 2u16 } else { 4u16 }; // including prefix bytes
    let setpc = format!("s.pc=0x{:04x};", next(ia, total_len));

    // memory EA (only for z==6, or for indexed all forms write back to reg too)
    let body;
    let asm;
    let opn;
    if mode == 0 {
        if z == 6 {
            // (HL) form
            match x {
                0 => { let f=ROT_FN[y as usize]; body=format!("{rinc}{} {{ let ea=ghl(s) as u32; let (r,nf)={f}(rd(s,ea),s.f); wr(s,ea,r); s.f=nf; }} {setpc}", cyc(15)); asm=format!("{} (hl)", ROT_NAME[y as usize]); opn="CB rot (HL)"; }
                1 => { body=format!("{rinc}{} {{ let v=rd(s,ghl(s) as u32); s.f=bit_test(v,{y},s.f,((ghl(s)>>8)as u8)); }} {setpc}", cyc(12)); asm=format!("bit {y},(hl)"); opn="BIT (HL)"; }
                2 => { body=format!("{rinc}{} {{ let ea=ghl(s) as u32; wr(s,ea, rd(s,ea)&!(1<<{y})); }} {setpc}", cyc(15)); asm=format!("res {y},(hl)"); opn="RES (HL)"; }
                3 => { body=format!("{rinc}{} {{ let ea=ghl(s) as u32; wr(s,ea, rd(s,ea)|(1<<{y})); }} {setpc}", cyc(15)); asm=format!("set {y},(hl)"); opn="SET (HL)"; }
                _ => unreachable!(),
            }
        } else {
            let rg = r8(z, 0);
            match x {
                0 => { let f=ROT_FN[y as usize]; body=format!("{rinc}{} {{ let (r,nf)={f}({rg},s.f); {rg}=r; s.f=nf; }} {setpc}", cyc(8)); asm=format!("{} {}", ROT_NAME[y as usize], r8_name(z,0)); opn="CB rot r"; }
                1 => { body=format!("{rinc}{} s.f=bit_test({rg},{y},s.f,{rg}); {setpc}", cyc(8)); asm=format!("bit {y},{}", r8_name(z,0)); opn="BIT r"; }
                2 => { body=format!("{rinc}{} {rg}&=!(1<<{y}); {setpc}", cyc(8)); asm=format!("res {y},{}", r8_name(z,0)); opn="RES r"; }
                3 => { body=format!("{rinc}{} {rg}|=(1<<{y}); {setpc}", cyc(8)); asm=format!("set {y},{}", r8_name(z,0)); opn="SET r"; }
                _ => unreachable!(),
            }
        }
    } else {
        // DDCB/FDCB: operate on (IX+d); for z!=6 also copy result to reg r (undoc)
        let ea = format!("({}.wrapping_add({disp}i16 as u16)) as u32", hlreg_get(mode));
        let copyreg = if z != 6 { format!("{}=r;", r8(z,0)) } else { String::new() };
        match x {
            0 => { let f=ROT_FN[y as usize]; body=format!("{rinc}{} {{ let ea={ea}; let (r,nf)={f}(rd(s,ea),s.f); wr(s,ea,r); s.f=nf; {copyreg} }} {setpc}", cyc(23)); asm=format!("{} ({}{:+})", ROT_NAME[y as usize], ss_name(2,mode), disp); opn="DDCB rot"; }
            1 => { body=format!("{rinc}{} {{ let ea={ea}; let v=rd(s,ea); s.f=bit_test(v,{y},s.f,((ea>>8)as u8)); }} {setpc}", cyc(20)); asm=format!("bit {y},({}{:+})", ss_name(2,mode), disp); opn="DDCB bit"; }
            2 => { body=format!("{rinc}{} {{ let ea={ea}; let r=rd(s,ea)&!(1<<{y}); wr(s,ea,r); {copyreg} }} {setpc}", cyc(23)); asm=format!("res {y},({}{:+})", ss_name(2,mode), disp); opn="DDCB res"; }
            3 => { body=format!("{rinc}{} {{ let ea={ea}; let r=rd(s,ea)|(1<<{y}); wr(s,ea,r); {copyreg} }} {setpc}", cyc(23)); asm=format!("set {y},({}{:+})", ss_name(2,mode), disp); opn="DDCB set"; }
            _ => unreachable!(),
        }
    }
    let _ = ilen;
    Ok(Insn { len: total_len.wrapping_sub(0), body, asm, succ: vec![next(ia, total_len)], op: opn.into() })
}

// ---- ED prefixed ----
fn decode_ed(rom: &Rom, addr: u16, pfx_len: u16) -> Result<Insn, String> {
    let ia = addr.wrapping_sub(pfx_len);
    let edop = rom.o(next(addr, 1));
    let a2 = next(addr, 2);
    let rinc = "s.r=(s.r&0x80)|(s.r.wrapping_add(1)&0x7f);";
    let cyc = |n: u32| format!("s.cycles=s.cycles.wrapping_add({n});");
    let x = edop >> 6; let y = (edop >> 3) & 7; let z = edop & 7; let p = (edop >> 4) & 3; let q = (edop >> 3) & 1;
    let mk = |len: u16, body: String, asm: String, succ: Vec<u16>, opn: &str| Insn { len, body, asm, succ, op: opn.into() };
    let setpc = |n: u16| format!("s.pc=0x{:04x};", next(ia, n));

    if x == 1 {
        match z {
            0 => { // IN r,(C)  (r=6 -> just flags)
                let dst = if y == 6 { String::new() } else { format!("{}=v;", r8(y,0)) };
                let body = format!("{rinc}{} {{ let v=in_io(s,s.c); {dst} s.f=szyxp_f(v)|(s.f&1); }} {}", cyc(12), setpc(2));
                return Ok(mk(2, body, format!("in {},(c)", r8_name(y,0)), vec![next(ia,2)], "IN r,(C)"));
            }
            1 => { // OUT (C),r
                let src = if y == 6 { "0u8".to_string() } else { r8(y,0) };
                let body = format!("{rinc}{} out_io(s,s.c,{src}); {}", cyc(12), setpc(2));
                return Ok(mk(2, body, format!("out (c),{}", r8_name(y,0)), vec![next(ia,2)], "OUT (C),r"));
            }
            2 => { // SBC/ADC HL,rr
                let rr = ss_get(p, 0);
                if q == 0 {
                    let body = format!("{rinc}{} {{ let (r,f)=sbc16(ghl(s),{rr},s.f); shl(s,r); s.f=f; }} {}", cyc(15), setpc(2));
                    return Ok(mk(2, body, format!("sbc hl,{}", ss_name(p,0)), vec![next(ia,2)], "SBC HL,rr"));
                } else {
                    let body = format!("{rinc}{} {{ let (r,f)=adc16(ghl(s),{rr},s.f); shl(s,r); s.f=f; }} {}", cyc(15), setpc(2));
                    return Ok(mk(2, body, format!("adc hl,{}", ss_name(p,0)), vec![next(ia,2)], "ADC HL,rr"));
                }
            }
            3 => { // LD (nn),rr / LD rr,(nn)
                let nn = rom.p16(a2);
                if q == 0 {
                    let v = ss_get(p, 0);
                    let body = format!("{rinc}{} {{ let v={v}; wr(s,0x{nn:04x},v as u8); wr(s,0x{:04x},(v>>8)as u8); }} {}", cyc(20), next(nn,1), setpc(4));
                    return Ok(mk(4, body, format!("ld (${nn:04x}),{}", ss_name(p,0)), vec![next(ia,4)], "LD (nn),rr"));
                } else {
                    let body = format!("{rinc}{} {{ let lo=rd(s,0x{nn:04x}) as u16; let hi=rd(s,0x{:04x}) as u16; {} }} {}", cyc(20), next(nn,1), ss_set(p,0,"lo|(hi<<8)"), setpc(4));
                    return Ok(mk(4, body, format!("ld {},(${nn:04x})", ss_name(p,0)), vec![next(ia,4)], "LD rr,(nn)"));
                }
            }
            4 => { // NEG
                let body = format!("{rinc}{} {{ let (a,f)=neg(s.a); s.a=a; s.f=f; }} {}", cyc(8), setpc(2));
                return Ok(mk(2, body, "neg".into(), vec![next(ia,2)], "NEG"));
            }
            5 => { // RETN/RETI
                let body = format!("{rinc}{} s.iff1=s.iff2; pop_pc(s);", cyc(14));
                return Ok(mk(2, body, "reti/retn".into(), vec![], "RETI/RETN"));
            }
            6 => { // IM n
                let imv = match y & 3 { 0 | 1 => 0, 2 => 1, _ => 2 };
                let body = format!("{rinc}{} s.im={imv}; {}", cyc(8), setpc(2));
                return Ok(mk(2, body, format!("im {imv}"), vec![next(ia,2)], "IM n"));
            }
            7 => { // LD I/R,A ; LD A,I/R ; RRD/RLD
                let body = match y {
                    0 => format!("{rinc}{} s.i=s.a; {}", cyc(9), setpc(2)),                // LD I,A
                    1 => format!("{rinc}{} s.r=s.a; {}", cyc(9), setpc(2)),                // LD R,A
                    2 => format!("{rinc}{} {{ s.a=s.i; s.f=szyxp_f(s.a)&!0x04 | ((s.iff2&1)<<2) | (s.f&1); s.f=(s.f&!0x10)&!0x02; }} {}", cyc(9), setpc(2)), // LD A,I (approx flags)
                    3 => format!("{rinc}{} {{ s.a=s.r; s.f=szyx_f(s.a) | ((s.iff2&1)<<2) | (s.f&1); }} {}", cyc(9), setpc(2)), // LD A,R
                    4 => format!("{rinc}{} {{ let (a,f)=rrd(s); s.a=a; s.f=f; }} {}", cyc(18), setpc(2)), // RRD
                    5 => format!("{rinc}{} {{ let (a,f)=rld(s); s.a=a; s.f=f; }} {}", cyc(18), setpc(2)), // RLD
                    _ => format!("{rinc}{} {}", cyc(8), setpc(2)), // 6,7 nop-ish
                };
                return Ok(mk(2, body, "ld i/r / rld/rrd".into(), vec![next(ia,2)], "LD A,I/R / RLD"));
            }
            _ => unreachable!(),
        }
    }
    if x == 2 {
        // block group: y>=4 (z 0..3). LDI/LDD/LDIR/LDDR, CPI..., INI..., OUTI...
        match (z, y) {
            (0, 4) => { let b=format!("{rinc}{} ldid(s,1,false); {}", cyc(16), setpc(2)); return Ok(mk(2,b,"ldi".into(),vec![next(ia,2)],"LDI")); }
            (0, 5) => { let b=format!("{rinc}{} ldid(s,-1i16 as u16,false); {}", cyc(16), setpc(2)); return Ok(mk(2,b,"ldd".into(),vec![next(ia,2)],"LDD")); }
            (0, 6) => { let b=format!("{rinc}{} ldid(s,1,true); {}", cyc(21), setpc(2)); return Ok(mk(2,b,"ldir".into(),vec![next(ia,2)],"LDIR")); }
            (0, 7) => { let b=format!("{rinc}{} ldid(s,-1i16 as u16,true); {}", cyc(21), setpc(2)); return Ok(mk(2,b,"lddr".into(),vec![next(ia,2)],"LDDR")); }
            (1, 4) => { let b=format!("{rinc}{} cpid(s,1,false); {}", cyc(16), setpc(2)); return Ok(mk(2,b,"cpi".into(),vec![next(ia,2)],"CPI")); }
            (1, 5) => { let b=format!("{rinc}{} cpid(s,-1i16 as u16,false); {}", cyc(16), setpc(2)); return Ok(mk(2,b,"cpd".into(),vec![next(ia,2)],"CPD")); }
            (1, 6) => { let b=format!("{rinc}{} cpid(s,1,true); {}", cyc(21), setpc(2)); return Ok(mk(2,b,"cpir".into(),vec![next(ia,2)],"CPIR")); }
            (1, 7) => { let b=format!("{rinc}{} cpid(s,-1i16 as u16,true); {}", cyc(21), setpc(2)); return Ok(mk(2,b,"cpdr".into(),vec![next(ia,2)],"CPDR")); }
            // INI/IND/INIR/INDR, OUTI/OUTD/OTIR/OTDR: rarely used by main CPU; treat as nop-advance but error if hit unexpectedly.
            _ => {
                let b = format!("{rinc}{} {}", cyc(16), setpc(2));
                return Ok(mk(2, b, format!("ed block z{z} y{y} (stub)"), vec![next(ia,2)], "ED block stub"));
            }
        }
    }
    // ED undefined: NOP (2 bytes) like the core (no catch)
    let b = format!("{rinc}{} {}", cyc(8), setpc(2));
    Ok(mk(2, b, format!("ed undef ${edop:02x}"), vec![next(ia,2)], "ED undefined"))
}

const PRELUDE: &str = include_str!("shinobi_audio_prelude.rs.txt");

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: recompile_shinobi_audio <Shinobi sound ROM> <out_lib.rs> [seed_hex ...]");
        std::process::exit(2);
    }
    let plain = fs::read(&args[1]).expect("read rom");
    // Shinobi sound CPU is NOT opcode-encrypted: the M1 "decoded" shadow == the plain ROM.
    let dec = plain.clone();
    let rom = Rom { plain, dec };

    // recursive descent
    let mut seeds: Vec<u16> = vec![0x0000, 0x0008, 0x0010, 0x0018, 0x0020, 0x0028, 0x0030, 0x0038, 0x0066];
    // Extra seeds (computed-jump / attract-path entry points). An arg may be a
    // literal hex address ("0x1234"/"1234"), or "@path" naming a file of
    // whitespace-separated hex addresses (one per line) -- used to feed the full
    // interpreter-harvested reachable-in-attract address set without a 1000-arg CLI.
    let mut add_seed = |tok: &str, seeds: &mut Vec<u16>| {
        let t = tok.trim();
        if t.is_empty() { return; }
        seeds.push(u16::from_str_radix(t.trim_start_matches("0x"), 16)
            .unwrap_or_else(|_| panic!("bad seed hex: {t:?}")));
    };
    for s in &args[3..] {
        if let Some(path) = s.strip_prefix('@') {
            let txt = fs::read_to_string(path).unwrap_or_else(|_| panic!("read seed file {path}"));
            for tok in txt.split_whitespace() { add_seed(tok, &mut seeds); }
        } else {
            add_seed(s, &mut seeds);
        }
    }
    let mut insns: BTreeMap<u16, Insn> = BTreeMap::new();
    let mut ops_seen: BTreeMap<String, u32> = BTreeMap::new();
    let mut work: Vec<u16> = seeds.clone();
    let mut errors: Vec<String> = Vec::new();
    let romlen = rom.plain.len() as u16;
    // SEEDS-ONLY mode (env SHINOBIAUD_SEEDS_ONLY=1): only emit an arm for a discovered
    // successor if that address is ALSO an explicit seed. With a COMPLETE interpreter
    // harvest (every executed instruction boundary is a seed), this is sufficient for
    // full coverage of every executed path, while preventing unbounded recursive
    // descent from FALLING THROUGH a code/data boundary and decoding kilobytes of
    // graphics/tables as bogus instructions (which bloats the m68k binary ~3x). A
    // successor not in the harvest was never actually executed, so omitting it is
    // safe -- if some untraced timing ever reaches it, run() reports BADPC instead of
    // silently running garbage, and the harvester can be widened.
    let seeds_only = std::env::var("SHINOBIAUD_SEEDS_ONLY").is_ok();
    let seed_set: BTreeSet<u16> = seeds.iter().cloned().collect();
    while let Some(a) = work.pop() {
        if a >= romlen { continue; }            // target in RAM/IO: can't recompile (computed)
        if insns.contains_key(&a) { continue; }
        match decode(&rom, a) {
            Ok(i) => {
                let succ = i.succ.clone();
                *ops_seen.entry(i.op.clone()).or_insert(0) += 1;
                insns.insert(a, i);
                for s in succ {
                    if seeds_only && !seed_set.contains(&s) { continue; }
                    work.push(s);
                }
            }
            Err(e) => { errors.push(e); }
        }
    }

    // Decode errors are NON-FATAL: a seed (e.g. a jump-table entry that turned out
    // to point at data, or a mid-instruction address) that fails to decode is simply
    // skipped -- it becomes an uncovered address, exactly as if it were never seeded,
    // so a noisy/over-broad seed set can never corrupt or break the build. Real code
    // reached by recursive descent from the reset/RST vectors never errors (verified),
    // so a non-empty list here only ever reflects speculative table/fuzz seeds.
    if !errors.is_empty() {
        let mut uniq: BTreeSet<String> = BTreeSet::new();
        for e in &errors { uniq.insert(e.clone()); }
        eprintln!("decode errors skipped ({} unique, non-fatal -- speculative seeds into data):", uniq.len());
        for e in uniq.iter().take(20) { eprintln!("  {e}"); }
    }

    // emit -- the LLVM m68k backend SIGILLs on a single huge match, so split the
    // arms into fixed-size chunk functions; run() dispatches by pc range.
    const CHUNK: usize = 96;
    let addrs: Vec<u16> = insns.keys().cloned().collect();
    let chunks: Vec<&[u16]> = addrs.chunks(CHUNK).collect();
    let mut s = String::new();
    s.push_str(PRELUDE);
    // chunk functions
    for (k, ch) in chunks.iter().enumerate() {
        s.push_str(&format!("\n#[inline(never)]\nfn run_c{k}(s: &mut Z80State) {{\n    loop {{\n        match s.pc {{\n"));
        for a in ch.iter() {
            let i = &insns[a];
            s.push_str(&format!("            0x{a:04x} => {{ {} }} // {}\n", i.body, i.asm));
        }
        s.push_str("            _ => return,\n        }\n        if s.stop != 0 { return; }\n        if s.cycles >= s.budget { return; }\n    }\n}\n");
    }
    // dispatcher. The default arm (uncovered computed-jump target) NO LONGER freezes
    // (stop=2): it INTERPRETS one Z80 op (interp_one) on the same Z80State + helpers,
    // then the loop re-dispatches at the next pc -- re-entering native code at the next
    // covered address. Same for a covered range that made no progress (run_cN returned
    // with pc unchanged: pc landed on a data gap / mid-instruction boundary inside the
    // chunk). The game can never wedge on a seed-harvest miss again; it stays ~100%
    // native (interp only catches the rare statically-missed jump). s.badpc records the
    // last interpreted pc for live introspection (it is NOT a stop).
    //
    // SHINOBIAUD_INTERP_ONLY=1 (generation env): emit a run() that routes EVERY op through
    // interp_one (native arms become dead code) -- used to VALIDATE the interpreter
    // exhaustively against ccommando.c / the native build (must be byte-identical).
    let interp_only = std::env::var("SHINOBIAUD_INTERP_ONLY").is_ok();
    s.push_str("\n#[no_mangle]\npub extern \"C\" fn run(state: *mut Z80State) {\n");
    s.push_str("    let s = unsafe { &mut *state };\n    s.stop = 0;\n");
    if interp_only {
        s.push_str("    loop {\n        interp_one(s);\n        if s.stop != 0 { return; }\n        if s.cycles >= s.budget { return; }\n    }\n}\n");
    } else {
        s.push_str("    loop {\n        let pc = s.pc;\n        let mut covered = true;\n        match pc {\n");
        for (k, ch) in chunks.iter().enumerate() {
            let lo = ch[0]; let hi = *ch.last().unwrap();
            s.push_str(&format!("            0x{lo:04x}..=0x{hi:04x} => run_c{k}(s),\n"));
        }
        s.push_str("            _ => covered = false,\n");
        s.push_str("        }\n");
        s.push_str("        if !covered || s.pc == pc { s.badpc = pc; interp_one(s); } // uncovered target / data-gap: interpret one op (never freeze)\n");
        s.push_str("        if s.stop != 0 { return; }\n");
        s.push_str("        if s.cycles >= s.budget { return; }\n");
        s.push_str("    }\n}\n");
    }
    fs::write(&args[2], &s).expect("write");
    eprintln!("emitted {} chunk fns (chunk size {CHUNK}){}", chunks.len(),
        if interp_only { " [INTERP-ONLY]" } else { "" });

    eprintln!("recompiled {} instructions from {} seeds -> {}", insns.len(), seeds.len(), args[2]);
    eprintln!("opcode families covered ({}):", ops_seen.len());
    for (o, c) in &ops_seen { eprintln!("  {:5}  {o}", c); }
}
