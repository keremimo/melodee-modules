#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""FM6's tables for the host table (MOD3_T_FM6_*): src/mm_fm6_tables.h from a Melodee firmware checkout's generated
build/gen/melodee_tables.h (tools/gen_tables.py there, run by its build): the seeds the firmware keeps in flash and
fills its RAM tables from at power-on (dsp_tables_init), so a module gets the device's tables bit for bit.
    tools/sync_fm6_tables.py [FIRMWARE_CHECKOUT]      (default ../Felucca)"""
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
FW = Path(sys.argv[1] if len(sys.argv) > 1 else HERE.parent / "Felucca").resolve()
GEN = FW / "build" / "gen" / "melodee_tables.h"
ARRAYS = [("FM6_MKI_LOG_H", "uint16_t", 1024), ("FM6_MKI_EXP_D", "uint8_t", 512), ("FM6_OPL_LOG_H", "uint16_t", 256),
          ("FM6_OPL_EXP_D", "uint8_t", 128)]

text = GEN.read_text()
out = ["/* SPDX-License-Identifier: Apache-2.0 AND GPL-3.0-or-later",
       " * FM6's lookup tables (MSFA's sine, Dexed's MARK I and OPL log-sine and exponent tables: LICENSES/Apache-2.0-msfa.txt",
       " * and Dexed's GPL-3.0-or-later; Copyright 2012 Google Inc., 2013-2025 Pascal Gauthier), as a Melodee firmware",
       " * makes them: written by tools/sync_fm6_tables.py from its build/gen/melodee_tables.h "
       f"({subprocess.run(['git', '-C', str(FW), 'log', '-1', '--format=%h', '--', 'tools/gen_tables.py'], capture_output=True, text=True).stdout.strip() or '?'}"
       "), do not edit. */",
       "#pragma once",
       "#include <stdint.h>", ""]
for name in ("FM6_SIN_C", "FM6_SIN_S"):
    m = re.search(rf"^#define {name} (-?\d+)$", text, re.M)
    if not m:
        raise SystemExit(f"{GEN}: no {name}")
    out.append(f"#define MM_{name} {m.group(1)}")
out.append("")
for name, ctype, n in ARRAYS:
    m = re.search(rf"static const {ctype} {name}\[{n}\] = \{{(.*?)\}};", text, re.S)
    if not m:
        raise SystemExit(f"{GEN}: no {ctype} {name}[{n}]")
    vals = [int(v) for v in m.group(1).replace("\n", " ").split(",") if v.strip()]
    if len(vals) != n:
        raise SystemExit(f"{GEN}: {name} has {len(vals)} values, not {n}")
    out.append(f"static const {ctype} MM_{name}[{n}] = {{")
    for i in range(0, n, 16):
        out.append("    " + ", ".join(str(v) for v in vals[i:i + 16]) + ",")
    out.append("};")
out += ["",
        "/* the RAM tables from the seeds, as the firmware's dsp_tables_init fills them: the sine by rotation (1025 points,",
        " * the last 0), MARK I's log (half a cycle) and exponent (4096 + exp, reversed), OPL's log and exponent ((0x400 +",
        " * exp) << 1, reversed) */",
        "static void mm_fm6_tables(float sin_t[1025], uint16_t mki_log[1024], uint16_t mki_exp[1024], uint16_t opl_log[256],",
        "                          uint16_t opl_exp[256])",
        "{",
        "    int32_t u = 1 << 30, v = 0, x;",
        "    uint32_t i, e;",
        "    for (i = 0; i < 512u; i++) {",
        "        x = (v + 32) >> 6;",
        "        sin_t[i] = (float)x * (1.0f / 16777216.0f);",
        "        sin_t[i + 512u] = (float)-x * (1.0f / 16777216.0f);",
        "        x = (int32_t)(((int64_t)u * MM_FM6_SIN_C - (int64_t)v * MM_FM6_SIN_S + (1 << 29)) >> 30);",
        "        v = (int32_t)(((int64_t)u * MM_FM6_SIN_S + (int64_t)v * MM_FM6_SIN_C + (1 << 29)) >> 30);",
        "        u = x;",
        "    }",
        "    sin_t[1024] = 0.0f;",
        "    for (i = 0; i < 1024u; i++)",
        "        mki_log[i] = MM_FM6_MKI_LOG_H[i];",
        "    for (i = 0; i < 256u; i++)",
        "        opl_log[i] = MM_FM6_OPL_LOG_H[i];",
        "    for (i = 0, e = 0u; i < 1024u; i++) {",
        "        if (i)",
        "            e += 2u + ((MM_FM6_MKI_EXP_D[(i - 1u) >> 1] >> (((i - 1u) & 1u) * 4u)) & 15u);",
        "        mki_exp[i ^ 1023u] = (uint16_t)(4096u + e);",
        "    }",
        "    for (i = 0, e = 0u; i < 256u; i++) {",
        "        if (i)",
        "            e += 2u + ((MM_FM6_OPL_EXP_D[(i - 1u) >> 1] >> (((i - 1u) & 1u) * 4u)) & 15u);",
        "        opl_exp[i ^ 255u] = (uint16_t)((0x400u + e) << 1);",
        "    }",
        "}", ""]
(HERE / "src" / "mm_fm6_tables.h").write_text("\n".join(out))
print(f"src/mm_fm6_tables.h from {GEN}")
