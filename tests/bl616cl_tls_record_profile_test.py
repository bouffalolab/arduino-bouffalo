#!/usr/bin/env python3
"""Keep the bridge TLS MFL request compatible with its SDK record buffers."""

from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
PROFILE = (
    ROOT_DIR
    
    / "tools" / "bridge_runtime"
    / "profiles"
    / "bridge"
    / "defconfig"
)
BACKEND = ROOT_DIR / "UNOR4BL616CLBridge" / "bouffalo_tls_client_backend.cpp"
COMPACT_CONFIG = PROFILE.parent / "mbedtls_bridge_p256_config.h"
GENERATOR = (
    ROOT_DIR
    
    / "tools" / "bridge_runtime"
    / "generate_runtime_bundle.py"
)


def main() -> int:
    profile = PROFILE.read_text(encoding="ascii")
    backend = BACKEND.read_text(encoding="ascii")
    compact_config = COMPACT_CONFIG.read_text(encoding="ascii")
    generator = GENERATOR.read_text(encoding="utf-8")
    if "CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN = 2048" not in profile:
        raise RuntimeError("bridge profile no longer declares 2048-byte TLS records")
    if "MBEDTLS_SSL_MAX_FRAG_LEN_1024" not in backend:
        raise RuntimeError("TLS client no longer requests 1 KiB established records")
    if "MBEDTLS_SSL_MAX_FRAG_LEN_2048" in backend:
        raise RuntimeError("TLS client still retains 2 KiB records after handshake")
    if "MBEDTLS_SSL_MAX_FRAG_LEN_4096" in backend:
        raise RuntimeError("TLS client still requests an incompatible 4096-byte MFL")
    if "MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH" not in backend:
        raise RuntimeError("TLS backend no longer requires variable record buffers")
    if 'CONFIG_MBEDTLS_USER_CONFIG_FILE    ="mbedtls_bridge_p256_config.h"' not in profile:
        raise RuntimeError("bridge profile does not select the compact mbedTLS config")
    for setting in (
        "#define MBEDTLS_MPI_WINDOW_SIZE 1",
        "#define MBEDTLS_MPI_MAX_SIZE 512",
        "#define MBEDTLS_ECP_WINDOW_SIZE 2",
        "#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0",
    ):
        if setting not in compact_config:
            raise RuntimeError(f"compact mbedTLS config is missing {setting}")
    if '"mbedtls_bridge_p256_config.h"' not in generator:
        raise RuntimeError("bundle generator does not stage the compact mbedTLS config")
    print("BL616CL_TLS_RECORD_PROFILE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
