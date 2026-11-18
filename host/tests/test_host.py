import json
import re
import zlib

import pytest

from aiready import DESCRIPTOR_JSON, REPO_ROOT, ihex
from aiready.block import BLOCK_ADDR, BLOCK_SIZE, HEADER_SIZE, pack, parse_header, unpack, to_hex_map
from aiready.board import BoardError, select, summary
from aiready.program import prepare_image
from aiready.stk500 import build_message, parse_message

DESC = json.loads(DESCRIPTOR_JSON.read_text())


# ---------------------------------------------------------------- descriptor block
def test_pack_roundtrip_and_fits():
    blk, did = pack(DESC)
    assert len(blk) <= BLOCK_SIZE
    assert b"!!!" not in blk
    h, obj = unpack(blk)
    assert h.id == did and obj == DESC


def test_block_starts_with_jump_to_bootloader():
    # AVR 'jmp k' = 1001 010k kkkk 110k + 16 low bits; k = word address 0x1F000 (byte 0x3E000)
    blk, _ = pack(DESC)
    w1, w2 = int.from_bytes(blk[0:2], "little"), int.from_bytes(blk[2:4], "little")
    assert w1 & 0xFE0E == 0x940C
    k = (((w1 >> 4) & 0x1F) << 17) | ((w1 & 1) << 16) | w2
    assert k * 2 == 0x3E000
    assert blk[4:8] == b"AIRD"


def test_crc_detects_corruption():
    blk = bytearray(pack(DESC)[0])
    blk[HEADER_SIZE + 10] ^= 0xFF
    with pytest.raises(ValueError, match="CRC"):
        unpack(bytes(blk))


def test_blank_flash_has_no_header():
    assert parse_header(b"\xff" * 32) is None
    with pytest.raises(ValueError, match="no descriptor"):
        unpack(b"\xff" * 64)


def test_oversize_rejected():
    import random
    rnd = random.Random(1)
    big = {"x": "".join(rnd.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(20000))}
    with pytest.raises(ValueError, match="too large"):
        pack(big)


def test_descriptor_has_required_sections():
    schema = json.loads((REPO_ROOT / "descriptor" / "schema.json").read_text())
    for key in schema["required"]:
        assert key in DESC, key
    for p in DESC["pins"]:
        assert {"pin", "name", "dir", "s"} <= set(p), p
        assert p["s"] in ("V", "R", "F", "?")


# ---------------------------------------------------------------- intel hex
def test_ihex_roundtrip_across_64k():
    mem = {0xFFF0 + i: i & 0xFF for i in range(64)}
    mem.update(to_hex_map(b"AIRD\x01\x00"))
    assert ihex.parse(ihex.dumps(mem)) == mem
    assert ihex.dumps(to_hex_map(b"A")).startswith(":020000040003F7\n:01A00000411E")


def test_ihex_bad_checksum():
    with pytest.raises(ValueError):
        ihex.parse(":0100000041BF\n")


def test_bang3_detection():
    assert ihex.contains_bang3(ihex.from_bytes(0x100, b"ab!!!c")) == [0x102]


# ---------------------------------------------------------------- image preparation
def test_prepare_strips_protected_and_trailing_blank():
    mem = ihex.from_bytes(0, b"\x0c\x94" * 300)        # 600 bytes of code
    mem.update(ihex.from_bytes(600, b"\xff" * 2000))    # padding
    mem.update(to_hex_map(b"AIRD"))                     # descriptor region
    mem.update(ihex.from_bytes(0x3E000, b"\x11" * 16))  # bootloader region
    img, notes = prepare_image(mem)
    assert max(img) < 768 and min(img) == 0
    assert all(a < BLOCK_ADDR for a in img)
    assert len(notes) == 3


def test_prepare_real_marlin_backup():
    path = REPO_ROOT / "firmware" / "backup" / "marlin_1.1.0-RC6_flash.hex"
    if not path.exists():
        pytest.skip("no backup")
    img, notes = prepare_image(ihex.load(path))
    assert max(img) < 0x19000 and any("bootloader" in n for n in notes)


# ---------------------------------------------------------------- stk500v2 framing
def test_stk500_sign_on_frame():
    # avrdude's CMD_SIGN_ON with seq 1: 1B 01 00 01 0E 01 checksum
    assert build_message(1, b"\x01").hex() == "1b0100010e0114"


def test_stk500_parse_reply_with_noise():
    reply = build_message(1, b"\x01\x00\x08AVRISP_2")
    seq, body, used = parse_message(b"\x00\x1bjunk" + reply)
    assert seq == 1 and body[3:] == b"AVRISP_2" and used == 6 + len(reply)


# ---------------------------------------------------------------- describe helpers
def test_select_paths():
    assert select(DESC, "pins.X_STEP")["pin"] == 54
    assert select(DESC, "pins.40")["name"] == "Y_MIN"
    assert select(DESC, "peripherals.thermistors.channels.0")["attached_to"].startswith("X stepper driver")
    with pytest.raises(BoardError):
        select(DESC, "nope")
    s = summary(DESC)
    assert "sections" in s and s["identity"]["profile"] == "cnc-poc-1"


# ---------------------------------------------------------------- header vs descriptor
def test_pin_header_matches_descriptor():
    hdr = (REPO_ROOT / "firmware" / "include" / "one_plus_pins.h").read_text()
    defs = {m[1]: int(m[2]) for m in re.finditer(r"#define\s+(\w+)\s+(\d+)\b", hdr)}
    pins = {p["name"]: p["pin"] for p in DESC["pins"]}
    pairs = {
        "X_STEP_PIN": "X_STEP", "X_DIR_PIN": "X_DIR", "X_ENABLE_PIN": "X_ENABLE",
        "Y_STEP_PIN": "Y_STEP", "Y_DIR_PIN": "Y_DIR", "Y_ENABLE_PIN": "Y_ENABLE",
        "Z_STEP_PIN": "Z_STEP", "Z_DIR_PIN": "Z_DIR", "Z_ENABLE_PIN": "Z_ENABLE",
        "E0_STEP_PIN": "E0_STEP", "E0_DIR_PIN": "E0_DIR", "E0_ENABLE_PIN": "E0_ENABLE",
        "X_MIN_PIN": "X_MIN", "X_MAX_PIN": "X_MAX", "Y_MIN_PIN": "Y_MIN", "Y_MAX_PIN": "Y_MAX",
        "Z_MIN_PIN": "Z_MIN", "Z_MAX_PIN": "Z_MAX", "HEATER_0_PIN": "HEATER_0",
        "HEATER_BED_PIN": "HEATER_BED", "FAN_PIN": "FAN", "LCD_CS_PIN": "LCD_CS",
        "LCD_A0_PIN": "LCD_A0", "LCD_RESET_PIN": "LCD_RESET", "BTN_EN1": "ENC_A",
        "BTN_EN2": "ENC_B", "BTN_ENC": "ENC_BTN", "BEEPER_PIN": "BEEPER", "SDSS": "SD_CS",
        "SD_DETECT_PIN": "SD_DETECT", "I2C_SDA_PIN": "I2C_SDA", "I2C_SCL_PIN": "I2C_SCL",
    }
    bad = {h: (defs.get(h), pins.get(d)) for h, d in pairs.items() if defs.get(h) != pins.get(d)}
    assert not bad, bad
    # thermistor ADC channels: pin = 54 + channel
    assert pins["TEMP0"] == 54 + defs["TEMP_0_ADC"] and pins["TEMP1"] == 54 + defs["TEMP_BED_ADC"]
