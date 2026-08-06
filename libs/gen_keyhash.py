#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# gen_keyhash.py <rsa_priv.pem> <out.h>
#
# From an MCUboot signing RSA private key (PKCS#8), derive the firmware-upgrade
# keyhash and emit a C header named udp_fw_keyhash.h for the udp_fw_upgrade lib.
#
# keyhash = SHA-256(RSA public key in PKCS#1/RSAPublicKey DER), which matches the
# IMG_TLV_KEYHASH (tag=0x0001) embedded in the signed image. Invoked from the
# library CMakeLists at configure time.

import sys
import hashlib
from cryptography.hazmat.primitives import serialization


def main():
    if len(sys.argv) < 3:
        print("usage: gen_keyhash.py <pem> <out.h> [symbol]", file=sys.stderr)
        return 1
    sym = sys.argv[3] if len(sys.argv) > 3 else "udp_fw_keyhash"
    upper = sym.upper()
    with open(sys.argv[1], "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    pub = key.public_key().public_bytes(
        serialization.Encoding.DER, serialization.PublicFormat.PKCS1)
    digest = hashlib.sha256(pub).digest()
    values = ", ".join("0x%02x" % b for b in digest)
    lines = [
        "#ifndef %s_H" % upper,
        "#define %s_H" % upper,
        "",
        "#include <stdint.h>",
        "",
        "#define %s_KEY_LEN 32" % upper,
        "",
        "static const uint8_t %s[%s_KEY_LEN] = {" % (sym, upper),
        "\t%s," % values,
        "};",
        "",
        "#endif /* %s_H */" % upper,
    ]
    with open(sys.argv[2], "w") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())