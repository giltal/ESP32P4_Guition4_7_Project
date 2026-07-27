/*
 * Per-network password store on the SD card — see bsp_wifi_store.h.
 */

#include "bsp_wifi_store.h"
#include "bsp_wifi.h"
#include "bsp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_wifi_store";

#define TMP_PATH  BSP_WIFI_STORE_PATH ".tmp"
#define LINE_MAX  (BSP_WIFI_SSID_MAX + BSP_WIFI_PASS_MAX + 8)

typedef struct {
    char ssid[BSP_WIFI_SSID_MAX + 1];
    char pass[BSP_WIFI_PASS_MAX + 1];
} entry_t;

/* ── Helpers ────────────────────────────────────────────────────── */
static bool storable(const char *s)
{
    /* A tab would be read back as a field break and a newline as a record
     * break, so neither can round-trip through this format. */
    return s != NULL && strchr(s, '\t') == NULL &&
                        strchr(s, '\n') == NULL &&
                        strchr(s, '\r') == NULL;
}

static void strip_eol(char *line)
{
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        line[--n] = '\0';
    }
}

/** Split "ssid<TAB>password" in place. Returns false for blanks and comments. */
static bool parse_line(char *line, const char **ssid, const char **pass)
{
    strip_eol(line);
    if (line[0] == '\0' || line[0] == '#') return false;

    char *tab = strchr(line, '\t');
    if (tab == NULL) return false;

    *tab  = '\0';
    *ssid = line;
    *pass = tab + 1;
    return (*ssid)[0] != '\0';
}

/** Read the whole file. Returns the number of entries read. */
static size_t load_all(entry_t *entries, size_t max)
{
    FILE *f = fopen(BSP_WIFI_STORE_PATH, "r");
    if (f == NULL) return 0;

    char   line[LINE_MAX];
    size_t n = 0;

    while (n < max && fgets(line, sizeof(line), f)) {
        const char *ssid, *pass;
        if (!parse_line(line, &ssid, &pass)) continue;

        strlcpy(entries[n].ssid, ssid, sizeof(entries[n].ssid));
        strlcpy(entries[n].pass, pass, sizeof(entries[n].pass));
        n++;
    }

    fclose(f);
    return n;
}

/**
 * Write via a temporary file and swap it in, so an interruption leaves the
 * previous file intact rather than a half-written one.
 */
static esp_err_t store_all(const entry_t *entries, size_t count)
{
    FILE *f = fopen(TMP_PATH, "w");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "cannot write %s", TMP_PATH);

    fprintf(f, "# gution wifi credentials\n");
    fprintf(f, "# <ssid>\\t<password> - plain text, keep the card safe\n");
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "%s\t%s\n", entries[i].ssid, entries[i].pass);
    }

    /* Flush to the card before the swap, or the rename can outrun the data. */
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* FATFS rename() will not overwrite, so drop the old file first. */
    remove(BSP_WIFI_STORE_PATH);
    if (rename(TMP_PATH, BSP_WIFI_STORE_PATH) != 0) {
        ESP_LOGE(TAG, "could not put %s in place", BSP_WIFI_STORE_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────── */
esp_err_t bsp_wifi_store_lookup(const char *ssid, char *out, size_t out_len)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0] && out && out_len,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(bsp_sdcard_is_mounted(), ESP_ERR_INVALID_STATE,
                        TAG, "no SD card");

    FILE *f = fopen(BSP_WIFI_STORE_PATH, "r");
    if (f == NULL) return ESP_ERR_NOT_FOUND;

    char      line[LINE_MAX];
    esp_err_t err = ESP_ERR_NOT_FOUND;

    while (fgets(line, sizeof(line), f)) {
        const char *s, *p;
        if (!parse_line(line, &s, &p)) continue;

        if (strcmp(s, ssid) == 0) {
            strlcpy(out, p, out_len);
            err = ESP_OK;
            break;
        }
    }

    fclose(f);
    return err;
}

bool bsp_wifi_store_has(const char *ssid)
{
    char pass[BSP_WIFI_PASS_MAX + 1];
    return bsp_wifi_store_lookup(ssid, pass, sizeof(pass)) == ESP_OK;
}

esp_err_t bsp_wifi_store_save(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "empty SSID");
    ESP_RETURN_ON_FALSE(bsp_sdcard_is_mounted(), ESP_ERR_INVALID_STATE,
                        TAG, "no SD card");

    if (password == NULL) password = "";

    if (!storable(ssid) || !storable(password)) {
        ESP_LOGW(TAG, "\"%s\" contains a tab or newline — not storable", ssid);
        return ESP_ERR_INVALID_ARG;
    }

    entry_t *entries = calloc(BSP_WIFI_STORE_MAX_ENTRIES, sizeof(entry_t));
    ESP_RETURN_ON_FALSE(entries, ESP_ERR_NO_MEM, TAG, "out of memory");

    size_t count = load_all(entries, BSP_WIFI_STORE_MAX_ENTRIES);

    /* Replace in place when the network is already known. */
    bool replaced = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].ssid, ssid) == 0) {
            strlcpy(entries[i].pass, password, sizeof(entries[i].pass));
            replaced = true;
            break;
        }
    }

    if (!replaced) {
        if (count >= BSP_WIFI_STORE_MAX_ENTRIES) {
            ESP_LOGW(TAG, "store is full (%d entries) — \"%s\" not saved",
                     BSP_WIFI_STORE_MAX_ENTRIES, ssid);
            free(entries);
            return ESP_ERR_NO_MEM;
        }
        strlcpy(entries[count].ssid, ssid, sizeof(entries[count].ssid));
        strlcpy(entries[count].pass, password, sizeof(entries[count].pass));
        count++;
    }

    esp_err_t err = store_all(entries, count);
    free(entries);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s password for \"%s\" (%u stored)",
                 replaced ? "Updated" : "Saved", ssid, (unsigned)count);
    }
    return err;
}

esp_err_t bsp_wifi_store_forget(const char *ssid)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "empty SSID");
    ESP_RETURN_ON_FALSE(bsp_sdcard_is_mounted(), ESP_ERR_INVALID_STATE,
                        TAG, "no SD card");

    entry_t *entries = calloc(BSP_WIFI_STORE_MAX_ENTRIES, sizeof(entry_t));
    ESP_RETURN_ON_FALSE(entries, ESP_ERR_NO_MEM, TAG, "out of memory");

    size_t count = load_all(entries, BSP_WIFI_STORE_MAX_ENTRIES);
    size_t kept  = 0;

    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].ssid, ssid) == 0) continue;
        if (kept != i) entries[kept] = entries[i];
        kept++;
    }

    esp_err_t err = ESP_OK;
    if (kept != count) {
        err = store_all(entries, kept);
        if (err == ESP_OK) ESP_LOGI(TAG, "Removed \"%s\"", ssid);
    }

    free(entries);
    return err;
}
