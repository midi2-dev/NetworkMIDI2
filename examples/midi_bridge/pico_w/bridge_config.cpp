#include "bridge_config.h"

#include <cstring>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"

namespace {

// Same technique as examples/midi_bridge/pico's bridge_config.cpp: a
// magic-tagged struct written to the last flash sector. Different magic
// value than that file's (and lwip/main.cpp's) FlashConfig -- the layouts
// differ (WiFi credentials instead of static-IP fields), and reusing the
// same magic over a different layout would let a board's flash content from
// one bridge variant be silently misread as valid config by another.
constexpr uint32_t kFlashMagic  = 0x57324D4Eu; // "NM2W"
constexpr uint32_t kFlashOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;

struct FlashConfig {
    uint32_t   magic;
    BridgeRole role;
    char       name[64];
    char       wifiSsid[33];
    char       wifiPassword[65];
    char       clientHostIp[16];
    uint16_t   clientHostPort;
};
static_assert(sizeof(FlashConfig) <= FLASH_PAGE_SIZE,
              "FlashConfig must fit in one flash page");

const FlashConfig *flashCfg() {
    return reinterpret_cast<const FlashConfig *>(XIP_BASE + kFlashOffset);
}

void copyStr(char *dst, size_t dstSize, const char *src, size_t srcSize) {
    size_t n = dstSize < srcSize ? dstSize : srcSize;
    memcpy(dst, src, n);
    dst[dstSize - 1] = '\0';
}

} // namespace

void loadBridgeConfig(BridgeConfig &cfg) {
    const FlashConfig *saved = flashCfg();
    if (saved->magic != kFlashMagic) return; // keep BridgeConfig{} defaults

    cfg.role    = saved->role;
    cfg.clientHostPort = saved->clientHostPort;
    copyStr(cfg.name, sizeof(cfg.name), saved->name, sizeof(saved->name));
    copyStr(cfg.wifiSsid, sizeof(cfg.wifiSsid), saved->wifiSsid, sizeof(saved->wifiSsid));
    copyStr(cfg.wifiPassword, sizeof(cfg.wifiPassword), saved->wifiPassword, sizeof(saved->wifiPassword));
    copyStr(cfg.clientHostIp, sizeof(cfg.clientHostIp), saved->clientHostIp, sizeof(saved->clientHostIp));
}

void saveBridgeConfig(const BridgeConfig &cfg) {
    FlashConfig out{};
    out.magic  = kFlashMagic;
    out.role   = cfg.role;
    out.clientHostPort = cfg.clientHostPort;
    copyStr(out.name, sizeof(out.name), cfg.name, sizeof(cfg.name));
    copyStr(out.wifiSsid, sizeof(out.wifiSsid), cfg.wifiSsid, sizeof(cfg.wifiSsid));
    copyStr(out.wifiPassword, sizeof(out.wifiPassword), cfg.wifiPassword, sizeof(cfg.wifiPassword));
    copyStr(out.clientHostIp, sizeof(out.clientHostIp), cfg.clientHostIp, sizeof(cfg.clientHostIp));

    // flash_range_program requires a whole number of flash pages; pad the
    // write buffer out to FLASH_PAGE_SIZE regardless of sizeof(FlashConfig).
    alignas(4) uint8_t page[FLASH_PAGE_SIZE] = {};
    memcpy(page, &out, sizeof(out));

    // Flash write requires interrupts disabled; safe here since this app is
    // single-core and the write happens outside the tud_task()/tick() loop.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
    flash_range_program(kFlashOffset, page, sizeof(page));
    restore_interrupts(ints);
}

bool parseDottedIp(const char *s, uint8_t out[4]) {
    unsigned a, b, c, d;
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = (uint8_t) a;
    out[1] = (uint8_t) b;
    out[2] = (uint8_t) c;
    out[3] = (uint8_t) d;
    return true;
}
