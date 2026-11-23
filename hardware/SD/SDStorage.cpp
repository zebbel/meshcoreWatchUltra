#include "SDStorage.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "board.h"
#include "board_pins.h"
#include "AXP2101.h"
#include "XL9555.h"

namespace hardware::sd {
    static const char *TAG = "SDStorage";
    static SemaphoreHandle_t mutex = nullptr;
    static SemaphoreHandle_t pending_mutex = nullptr;
    static sdmmc_card_t *card = nullptr;
    static bool initialized = false;
    static bool mounted = false;
    static int nesting = 0;
    static uint32_t last_capacity_kib = 0;
    static bool established_card = false;
    static uint32_t card_serial = 0;
    static bool storage_fault = false;
    static bool cached_present = false;
    static uint32_t last_detect_ms = 0;
    // Automatically learned contact advert updates are coalesced in RAM for 30 minutes.
    static constexpr uint32_t DEFER_MS = 30U * 60U * 1000U;
    static constexpr size_t MAX_DEFERRED = 64;

    struct Deferred {
        bool used = false;
        uint32_t queued_ms = 0;
        char path[MAX_PATH] = {};
        size_t length = 0;
        uint8_t bytes[MAX_DEFERRED_BYTES] = {};
    };
    static Deferred pending[MAX_DEFERRED] = {};
    static uint32_t last_flush_attempt = 0;

    static bool valid_path(const char *path){
        return path != nullptr && strncmp(path, "/sdcard/", 8) == 0 && strlen(path) < MAX_PATH && strstr(path, "..") == nullptr;
    }

    bool card_present(){
        bool present = false;
        const esp_err_t err = XL9555::is_sd_inserted(present);
        if(err != ESP_OK) present = false;
        // Removal after first mount invalidates this boot session. The user
        // must reboot to revalidate the card and its MeshCore identity.
        if(established_card && cached_present && !present) storage_fault = true;
        cached_present = present;
        last_detect_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
        return present;
    }

    bool ready(){
        if(!initialized || storage_fault) return false;
        const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if(static_cast<uint32_t>(now - last_detect_ms) > 2000) card_present();
        return cached_present && !storage_fault;
    }
    bool writable(){ return ready(); }
    uint32_t capacity_kib(){ return last_capacity_kib; }

    static esp_err_t power_off(){
        // SDSPI may release the GPIO21 output configuration at unmount.
        // Restore deselected CS before switching off the SD power rail.
        // Do not reconfigure the shared MOSI/MISO/SCLK pins.
        // SDSPI may leave GPIO21 reserved after unmount. Release its old
        // ownership before restoring the known-good output-high idle state.
        // gpio_reset_pin() enables a pull-up while output is disabled.
        esp_err_t err = gpio_reset_pin(board::pins::SD_CS);
        if(err != ESP_OK) return err;
        err = gpio_set_level(board::pins::SD_CS, 1); // Set latch before enabling output.
        if(err != ESP_OK) return err;
        err = gpio_set_direction(board::pins::SD_CS, GPIO_MODE_OUTPUT);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "SD CS output restore failed: %s", esp_err_to_name(err));
            return err; // Do not power off without a known deselected CS.
        }
        // gpio_get_level() would return 0 in GPIO_MODE_OUTPUT, even if the
        // output is high (input buffer disabled): do not use it as verification.
        const esp_err_t off_err = AXP2101::disable(AXP2101::rail_t::sd);
        vTaskDelay(pdMS_TO_TICKS(2));
        return off_err;
    }

    // ESP-IDF 6.1.1 SDSPI cleanup changes SD CS to INPUT but can leave
    // its output reservation active, generating a GPIO conflict warning.
    // Call only after the last file operation, under the exclusive SD/SPI
    // session lock. No SDSPI transfers are permitted after this handoff.
    // gpio_reset_pin() publicly releases the reservation; re-enable GPIO
    // output through gpio_set_direction() (which does not re-reserve it),
    // keeping the SD card safely deselected until the SDK unmounts it.
    static esp_err_t release_sd_cs_ownership_for_unmount(){
        esp_err_t err = gpio_set_level(board::pins::SD_CS, 1);
        if(err != ESP_OK) return err;
        err = gpio_reset_pin(board::pins::SD_CS); // Releases ESP-IDF GPIO reservation.
        if(err != ESP_OK) return err;
        err = gpio_set_level(board::pins::SD_CS, 1); // Preload latch high.
        if(err != ESP_OK) return err;
        return gpio_set_direction(board::pins::SD_CS, GPIO_MODE_OUTPUT);
    }

    static esp_err_t mount(){
        if(!card_present()) return ESP_ERR_NOT_FOUND;
        // Release the idle GPIO21 output reservation before SDSPI attaches.
        // Its internal pull-up maintains CS deasserted during card power-up.
        esp_err_t err = gpio_reset_pin(board::pins::SD_CS);
        if(err != ESP_OK) return err;
        err = gpio_set_level(board::pins::SD_CS, 1);
        if(err != ESP_OK) return err;
        err = AXP2101::enable(AXP2101::rail_t::sd);
        if(err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(15));

        // SPI2 is already initialized by board::init(). Never reinitialize it.
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = board::spi_host();
        host.max_freq_khz = 10000;
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id = board::spi_host();
        slot.gpio_cs = board::pins::SD_CS;
        esp_vfs_fat_mount_config_t cfg = {};
        cfg.format_if_mount_failed = false;
        cfg.max_files = 5;
        cfg.allocation_unit_size = 4096;
        err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &cfg, &card);
        if(err != ESP_OK){
            ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(err));
            card = nullptr;
            power_off();
            return err;
        }
        // Reject a different physical card after the initial successful mount.
        if(established_card && card->cid.serial != card_serial){
            ESP_LOGE(TAG, "A different SD card was inserted; reboot required");
            const esp_err_t cs_err = release_sd_cs_ownership_for_unmount();
            if(cs_err != ESP_OK){
                ESP_LOGW(TAG, "SD CS cleanup before unmount failed: %s", esp_err_to_name(cs_err));
            }
            esp_vfs_fat_sdcard_unmount("/sdcard", card);
            card = nullptr;
            power_off();
            return ESP_ERR_INVALID_STATE;
        }
        if(!established_card){
            card_serial = card->cid.serial;
            established_card = true;
        }
        mounted = true;
        last_capacity_kib = static_cast<uint32_t>((static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size) / 1024U);
        return ESP_OK;
    }

    static void unmount(){
        if(mounted){
            const esp_err_t cs_err = release_sd_cs_ownership_for_unmount();
            if(cs_err != ESP_OK){
                ESP_LOGW(TAG, "SD CS cleanup before unmount failed: %s", esp_err_to_name(cs_err));
            }
            esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", card);
            if(err != ESP_OK){
                ESP_LOGE(TAG, "Unmount failed: %s", esp_err_to_name(err));
                storage_fault = true;
            }
            card = nullptr;
            mounted = false;
        }
        if(power_off() != ESP_OK) storage_fault = true;
    }

    static void worker(void *){
        while(true){
            // MCW_ENERGY_STAGE2_V1: 30-second cadence for infrequent SD batches.
            // Deferred writes still coalesce; explicit writes stay immediate.
            vTaskDelay(pdMS_TO_TICKS(30000));
            if(pending_mutex == nullptr) continue;
            if(xSemaphoreTake(pending_mutex, pdMS_TO_TICKS(100)) != pdTRUE) continue;
            const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
            bool due = false;
            for(const auto &p : pending){
                if(p.used && static_cast<uint32_t>(now - p.queued_ms) >= DEFER_MS) due = true;
            }
            xSemaphoreGive(pending_mutex);
            if(due && static_cast<uint32_t>(now - last_flush_attempt) >= 5000){
                last_flush_attempt = now;
                esp_err_t err = flush_pending();
                if(err != ESP_OK) ESP_LOGW(TAG, "Deferred save pending: %s", esp_err_to_name(err));
            }
        }
    }

    esp_err_t init(){
        if(initialized) return ESP_OK;
        mutex = xSemaphoreCreateRecursiveMutex();
        pending_mutex = xSemaphoreCreateMutex();
        if(mutex == nullptr || pending_mutex == nullptr) return ESP_ERR_NO_MEM;
        cached_present = card_present();
        const BaseType_t rc = xTaskCreate(worker, "sd-saver", 12288, nullptr, 3, nullptr);
        if(rc != pdPASS) return ESP_ERR_NO_MEM;
        initialized = true;
        // Do not switch on ALDO1 here; startup storage session will mount.
        return ESP_OK;
    }

    Session::Session(){
        if(!initialized || mutex == nullptr){ error_ = ESP_ERR_INVALID_STATE; return; }
        if(xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(10000)) != pdTRUE){ error_ = ESP_ERR_TIMEOUT; return; }
        locked_ = true;
        if(nesting++ != 0){
            ok_ = mounted;
            error_ = ok_ ? ESP_OK : ESP_ERR_INVALID_STATE;
            return;
        }
        if(!board::spi_lock(pdMS_TO_TICKS(10000))){
            --nesting;
            locked_ = false;
            xSemaphoreGiveRecursive(mutex);
            error_ = ESP_ERR_TIMEOUT;
            return;
        }
        error_ = mount();
        if(error_ == ESP_OK) ok_ = true;
        else {
            --nesting;
            board::spi_unlock();
            locked_ = false;
            xSemaphoreGiveRecursive(mutex);
        }
    }

    Session::~Session(){ close(); }

    void Session::close(){
        if(!locked_) return;
        if(--nesting == 0){
            unmount();
            board::spi_unlock();
        }
        locked_ = false;
        ok_ = false;
        xSemaphoreGiveRecursive(mutex);
    }

    static esp_err_t mkdir_if_missing(const char *path){
        if(mkdir(path, 0755) == 0 || errno == EEXIST) return ESP_OK;
        return ESP_FAIL;
    }

    esp_err_t ensure_directory(const char *path){
        if(!valid_path(path)) return ESP_ERR_INVALID_ARG;
        Session session;
        if(!session.ok()) return session.error();
        return mkdir_if_missing(path);
    }

    bool exists(const char *path){
        if(!valid_path(path)) return false;
        Session session;
        if(!session.ok()) return false;
        struct stat statbuf = {};
        return stat(path, &statbuf) == 0;
    }

    static esp_err_t recover_file(const char *path){
        char bak[MAX_PATH + 5];
        char tmp[MAX_PATH + 5];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        struct stat s = {};
        if(stat(path, &s) == 0){
            // Main exists: remove abandoned old temporary file.
            unlink(tmp);
            return ESP_OK;
        }
        if(stat(bak, &s) == 0){
            if(rename(bak, path) != 0) return ESP_FAIL;
            unlink(tmp);
            return ESP_OK;
        }
        // An orphan .tmp is NOT sufficient proof of an authorized new record.
        if(stat(tmp, &s) == 0) return ESP_ERR_INVALID_CRC;
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t read_file(const char *path, void *out, size_t capacity, size_t &length){
        length = 0;
        if(!valid_path(path) || out == nullptr || capacity == 0) return ESP_ERR_INVALID_ARG;
        Session session;
        if(!session.ok()) return session.error();
        esp_err_t recover = recover_file(path);
        if(recover != ESP_OK) return recover;
        FILE *f = fopen(path, "rb");
        if(f == nullptr) return ESP_FAIL;
        const size_t n = fread(out, 1, capacity + 0 /* bounded */, f);
        if(ferror(f)){
            fclose(f);
            return ESP_FAIL;
        }
        const int next = fgetc(f);
        fclose(f);
        if(next != EOF) return ESP_ERR_INVALID_SIZE;
        length = n;
        return ESP_OK;
    }

    static esp_err_t write_raw(const char *path, const void *bytes, size_t length){
        char tmp[MAX_PATH + 5];
        char bak[MAX_PATH + 5];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        snprintf(bak, sizeof(bak), "%s.bak", path);
        FILE *f = fopen(tmp, "wb");
        if(f == nullptr) return ESP_FAIL;
        const bool complete = fwrite(bytes, 1, length, f) == length;
        const bool flushed = fflush(f) == 0;
        const bool synced = fsync(fileno(f)) == 0;
        const bool closed = fclose(f) == 0;
        if(!complete || !flushed || !synced || !closed){ unlink(tmp); return ESP_FAIL; }
        struct stat st = {};
        const bool had_old = stat(path, &st) == 0;
        if(had_old){
            unlink(bak);
            if(rename(path, bak) != 0) return ESP_FAIL;
        }
        if(rename(tmp, path) != 0){
            if(had_old) rename(bak, path);
            return ESP_FAIL;
        }
        // Keep .bak until next update? On FAT, metadata may be lost during power cut.
        // Old copy is retained as a recovery candidate if main disappears.
        return ESP_OK;
    }

    esp_err_t write_file(const char *path, const void *bytes, size_t length){
        if(!valid_path(path) || (bytes == nullptr && length > 0)) return ESP_ERR_INVALID_ARG;
        Session session;
        if(!session.ok()) return session.error();
        esp_err_t err = write_raw(path, bytes, length);
        return err;
    }

    esp_err_t remove_file(const char *path){
        if(!valid_path(path)) return ESP_ERR_INVALID_ARG;
        Session session;
        if(!session.ok()) return session.error();
        char bak[MAX_PATH + 5];
        char tmp[MAX_PATH + 5];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        unlink(tmp);
        unlink(bak);
        if(unlink(path) == 0 || errno == ENOENT) return ESP_OK;
        return ESP_FAIL;
    }

    bool has_pending_writes(){
        if(pending_mutex == nullptr || xSemaphoreTake(pending_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
        bool any = false;
        for(const auto &p : pending) if(p.used){ any = true; break; }
        xSemaphoreGive(pending_mutex);
        return any;
    }

    esp_err_t defer_write(const char *path, const void *data, size_t length){
        if(!valid_path(path) || data == nullptr || length == 0 || length > MAX_DEFERRED_BYTES) return ESP_ERR_INVALID_ARG;
        if(!ready()) return ESP_ERR_INVALID_STATE;
        if(xSemaphoreTake(pending_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        Deferred *slot = nullptr;
        for(auto &p : pending) if(p.used && strcmp(p.path, path) == 0){ slot = &p; break; }
        if(slot == nullptr) for(auto &p : pending) if(!p.used){ slot = &p; break; }
        if(slot == nullptr){ xSemaphoreGive(pending_mutex); return ESP_ERR_NO_MEM; }
        if(!slot->used){
            slot->queued_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
            snprintf(slot->path, sizeof(slot->path), "%s", path);
        }
        memcpy(slot->bytes, data, length);
        slot->length = length;
        slot->used = true;
        xSemaphoreGive(pending_mutex);
        return ESP_OK;
    }

    esp_err_t flush_pending(){
        if(pending_mutex == nullptr || xSemaphoreTake(pending_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        bool any = false;
        for(const auto &p : pending) if(p.used) any = true;
        if(!any){ xSemaphoreGive(pending_mutex); return ESP_OK; }
        Session session;
        if(!session.ok()){
            xSemaphoreGive(pending_mutex);
            return session.error();
        }
        for(auto &p : pending){
            if(!p.used) continue;
            esp_err_t err = write_raw(p.path, p.bytes, p.length);
            if(err != ESP_OK){ xSemaphoreGive(pending_mutex); return err; }
            p.used = false;
        }
        xSemaphoreGive(pending_mutex);
        return ESP_OK;
    }

    esp_err_t cancel_pending(const char *path){
        if(pending_mutex == nullptr) return ESP_ERR_INVALID_STATE;
        if(xSemaphoreTake(pending_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        for(auto &p : pending) if(p.used && strcmp(p.path, path) == 0) p.used = false;
        xSemaphoreGive(pending_mutex);
        return ESP_OK;
    }
}
