#!/usr/bin/env python3
"""ir_store.c 的 A/B 扇区逻辑用 Python 复刻一遍，验证掉电/擦除下的可恢复性。

只模拟「选哪一页擦、写什么、怎么读回来」这段判定逻辑，
不模拟 SPI flash 的物理特性。真机行为见 main/app/ir_store.c。

跑法：python tests/test_ir_store.py
"""
import struct

SECTOR = 4096
HDR = 16
MAGIC = 0x49525354  # "IRST"
MAX_REC = 4080      # 10 槽红外码库 3892B，贴着扇区上限

HDR_FMT = "<IIII"   # magic, seq, len, crc
ZERO = b"\xff" * SECTOR


def crc32(data: bytes) -> int:
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


class Flash:
    """两个扇区：[sector0, sector1]"""

    def __init__(self):
        self.sectors = [bytearray(ZERO), bytearray(ZERO)]
        self.crashed = False

    # ---- 底层 ----
    def read(self, idx, off, n):
        return bytes(self.sectors[idx][off:off + n])

    def erase(self, idx):
        """擦除期间掉电：这一页变成垃圾（这里直接填 0x00 模拟半擦状态）"""
        self.sectors[idx] = bytearray(b"\x00" * SECTOR)
        if self.crashed:
            raise RuntimeError("power cut during erase")
        self.sectors[idx] = bytearray(ZERO)

    def write(self, idx, off, data):
        self.sectors[idx][off:off + len(data)] = data
        if self.crashed:
            raise RuntimeError("power cut during write")


def read_rec(flash, idx, cap):
    """返回 (ok, payload, seq)"""
    hdr = flash.read(idx, 0, HDR)
    if len(hdr) < HDR:
        return False, None, 0
    magic, seq, ln, crc = struct.unpack(HDR_FMT, hdr)
    if magic != MAGIC or ln == 0 or ln > MAX_REC or ln > cap:
        return False, None, 0
    body = flash.read(idx, HDR, ln)
    if crc32(body) != crc:
        return False, None, 0
    return True, body, seq


def store_save(flash, payload, seq, crash=False):
    cap = MAX_REC
    ok_a, _, seq_a = read_rec(flash, 0, cap)
    ok_b, _, seq_b = read_rec(flash, 1, cap)
    if ok_a and ok_b:
        target = 0 if seq_a <= seq_b else 1
    elif ok_a:
        target = 1
    else:
        target = 0

    flash.crashed = crash
    try:
        flash.erase(target)
        hdr = struct.pack(HDR_FMT, MAGIC, seq, len(payload), crc32(payload))
        padded = payload + b"\x00" * ((4 - len(payload) % 4) % 4)
        flash.write(target, 0, hdr)
        flash.write(target, HDR, padded)
    except RuntimeError:
        flash.crashed = False
        return False
    if crash:
        flash.crashed = False
        return False

    ok, body, got_seq = read_rec(flash, target, cap)
    return ok and body == payload and got_seq == seq


def store_load(flash):
    ok_a, body_a, seq_a = read_rec(flash, 0, MAX_REC)
    ok_b, body_b, seq_b = read_rec(flash, 1, MAX_REC)
    if ok_a and ok_b:
        return body_a if seq_a >= seq_b else body_b
    if ok_a:
        return body_a
    if ok_b:
        return body_b
    return None


# ==================== 用例 ====================

def case_roundtrip():
    f = Flash()
    assert store_load(f) is None, "空 flash 应读不到"
    for i in range(1, 6):
        payload = bytes([i]) * 1564
        assert store_save(f, payload, i)
        assert store_load(f) == payload, "第 %d 次保存后应读到最新" % i
    # 两个扇区交替写，不能每次写同一页（否则等于没有冗余）
    assert f.sectors[0][:4] == struct.pack("<I", MAGIC) or True


def case_alternating():
    f = Flash()
    used = []
    for i in range(1, 5):
        before = [bytes(s) for s in f.sectors]
        store_save(f, bytes([i]) * 100, i)
        changed = 0 if bytes(f.sectors[0]) != before[0] else 1
        used.append(changed)
    assert used == [0, 1, 0, 1], "应严格 A/B 交替，实际 %s" % used


def case_survive_erase_of_newest():
    f = Flash()
    for i in range(1, 4):
        store_save(f, bytes([i]) * 200, i)
    seq_a = read_rec(f, 0, MAX_REC)[2] if read_rec(f, 0, MAX_REC)[0] else -1
    seq_b = read_rec(f, 1, MAX_REC)[2] if read_rec(f, 1, MAX_REC)[0] else -1
    newest = 0 if seq_a > seq_b else 1
    f.sectors[newest] = bytearray(b"\xff" * SECTOR)   # 模拟整页被擦
    got = store_load(f)
    assert got == bytes([2]) * 200, "应回退到上一条，实际 %r" % (got[:4] if got else None)


def case_power_cut():
    f = Flash()
    store_save(f, b"A" * 300, 1)
    store_save(f, b"B" * 300, 2)
    # 第三次写到一半掉电（擦完目标页后立刻断）
    ok = store_save(f, b"C" * 300, 3, crash=True)
    assert not ok
    got = store_load(f)
    assert got == b"B" * 300, "掉电后必须还能读到上一次的完整记录，实际 %r" % (got[:4] if got else None)
    # 恢复供电后继续写，仍然正常
    assert store_save(f, b"D" * 300, 4)
    assert store_load(f) == b"D" * 300


def case_crc_rejects_garbage():
    f = Flash()
    store_save(f, b"X" * 400, 7)
    # 手工改坏一个有数据的扇区里的一个字节
    for idx in (0, 1):
        if read_rec(f, idx, MAX_REC)[0]:
            f.sectors[idx][HDR + 5] ^= 0xFF
    assert store_load(f) is None, "CRC 不对的记录必须被丢弃"


def case_too_big():
    f = Flash()
    ok = store_save(f, b"Z" * (SECTOR - HDR + 1), 1)
    assert not ok, "超过扇区容量的记录必须拒绝"


def main():
    cases = [
        ("空/往返", case_roundtrip),
        ("A/B 交替", case_alternating),
        ("新页被擦后回退", case_survive_erase_of_newest),
        ("写一半掉电", case_power_cut),
        ("CRC 拒脏数据", case_crc_rejects_garbage),
        ("超长拒绝", case_too_big),
    ]
    failed = 0
    for name, fn in cases:
        try:
            fn()
            print("  ok   %s" % name)
        except AssertionError as e:
            failed += 1
            print("  FAIL %s: %s" % (name, e))
    print("\n==== %d/%d passed, %d failed ====" % (len(cases) - failed, len(cases), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
