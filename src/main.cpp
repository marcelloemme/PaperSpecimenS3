#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include "web_content.h"

// Embedded default fonts (extracted to flash on first boot without SD)
#include "embedded_apfel_grotezk_bold.h"
#include "embedded_ortica_linear_light.h"
#include "embedded_ronzino_regular.h"

// FreeType headers (provided by OpenFontRender)
#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_BBOX_H
#include FT_MODULE_H

// CFF/OTF support: manually register modules since ftmodule.h patching
// is unreliable with PlatformIO's build caching
extern "C" {
    extern const FT_Module_Class  psnames_module_class;
    extern const FT_Module_Class  psaux_module_class;
    extern const FT_Module_Class  pshinter_module_class;
    extern void* get_cff_driver_class(void);
}

static void registerCFFModules(FT_Library lib) {
    FT_Error e;
    e = FT_Add_Module(lib, &psnames_module_class);
    Serial.printf("  Add psnames: 0x%02X\n", e);
    e = FT_Add_Module(lib, &psaux_module_class);
    Serial.printf("  Add psaux: 0x%02X\n", e);
    e = FT_Add_Module(lib, &pshinter_module_class);
    Serial.printf("  Add pshinter: 0x%02X\n", e);
    e = FT_Add_Module(lib, (const FT_Module_Class*)get_cff_driver_class());
    Serial.printf("  Add cff: 0x%02X\n", e);
    Serial.println("CFF/OTF modules registered");
}

// PaperSpecimen S3 - v5.1.0
static const char* VERSION = "v5.1.0";

// Flash font storage threshold (11.5MB)
#define FLASH_FONT_MAX_BYTES (11.5 * 1024 * 1024)
// Flag: are fonts stored in flash (LittleFS) or SD?
static bool fontsInFlash = false;
static bool flashInitialized = false;

// Forward declarations
void runWiFiFontManager();

// M5PaperS3 SD Card SPI pins
#define SD_SPI_CS_PIN   47
#define SD_SPI_SCK_PIN  39
#define SD_SPI_MOSI_PIN 38
#define SD_SPI_MISO_PIN 40

// Target glyph size (like original PaperSpecimen's 400px)
#define TARGET_GLYPH_SIZE 420
// Label size in pixels (rendered with current font via FreeType)
#define LABEL_PIXEL_SIZE 24
// Uniform padding for UI and glyph labels
#define UI_PAD       30
// Config file path
#define CONFIG_FILE "/.paperspecimen.cfg"

// Max fonts (used by config and font list)
#define MAX_FONTS 100

// ---- Unicode ranges (same 28 as original PaperSpecimen) ----

struct UnicodeRange {
    uint32_t start;
    uint32_t end;
    const char* name;
};

static const UnicodeRange glyphRanges[] = {
    // First 6 enabled by default
    {0x0041, 0x005A, "Latin Uppercase"},
    {0x0061, 0x007A, "Latin Lowercase"},
    {0x0030, 0x0039, "Digits"},
    {0x0021, 0x002F, "Basic Punctuation"},
    {0x00A1, 0x00BF, "Latin-1 Punctuation"},
    {0x00C0, 0x00FF, "Latin-1 Letters"},
    // Disabled by default
    {0x0100, 0x017F, "Latin Extended-A"},
    {0x0180, 0x024F, "Latin Extended-B"},
    {0x0370, 0x03FF, "Greek and Coptic"},
    {0x0400, 0x04FF, "Cyrillic"},
    {0x0590, 0x05FF, "Hebrew"},
    {0x0600, 0x06FF, "Arabic"},
    {0x0900, 0x097F, "Devanagari"},
    {0x0E00, 0x0E7F, "Thai"},
    {0x10A0, 0x10FF, "Georgian"},
    {0x3040, 0x309F, "Hiragana"},
    {0x30A0, 0x30FF, "Katakana"},
    {0x4E00, 0x9FFF, "CJK Ideographs"},
    {0xAC00, 0xD7AF, "Hangul"},
    {0x2000, 0x206F, "General Punctuation"},
    {0x20A0, 0x20CF, "Currency Symbols"},
    {0x2100, 0x214F, "Letterlike Symbols"},
    {0x2190, 0x21FF, "Arrows"},
    {0x2200, 0x22FF, "Math Operators"},
    {0x2500, 0x257F, "Box Drawing"},
    {0x2580, 0x259F, "Block Elements"},
    {0x25A0, 0x25FF, "Geometric Shapes"},
    {0x2600, 0x26FF, "Misc Symbols"},
};
static const int NUM_GLYPH_RANGES = sizeof(glyphRanges) / sizeof(glyphRanges[0]);

// Font list (declared early, needed by config functions)
static String fontPaths[MAX_FONTS];
static String fontNames[MAX_FONTS];
static int fontCount = 0;
static int currentFontIndex = 0;
static uint32_t currentCodepoint = 'A';

// ---- Configuration ----

struct AppConfig {
    uint8_t wakeIntervalMinutes;       // 5, 10, or 15
    bool allowDifferentFont;           // Random font on wake
    bool allowDifferentMode;           // Random mode (bitmap/outline) on wake
    bool isDebugMode;                  // Debug mode (persisted across wakes)
    bool flipInterface;                // Rotate display 180° (for lanyard use)
    bool fontEnabled[MAX_FONTS];       // Per-font enable
    bool rangeEnabled[NUM_GLYPH_RANGES]; // Per-range enable
};

static AppConfig config;

// ---- Config save/load ----

void initDefaultConfig() {
    config.wakeIntervalMinutes = 15;
    config.allowDifferentFont = true;
    config.allowDifferentMode = true;
    config.isDebugMode = false;
    config.flipInterface = false;
    for (int i = 0; i < MAX_FONTS; i++) config.fontEnabled[i] = true;
    for (int i = 0; i < NUM_GLYPH_RANGES; i++) config.rangeEnabled[i] = (i < 6);
}

// Open a file from the active filesystem (flash or SD)
File openFromStorage(const char* path, const char* mode) {
    if (fontsInFlash && flashInitialized) {
        return LittleFS.open(path, mode);
    }
    return SD.open(path, mode);
}

bool storageExists(const char* path) {
    if (fontsInFlash && flashInitialized) {
        return LittleFS.exists(path);
    }
    return SD.exists(path);
}

bool loadConfig() {
    Serial.printf("loadConfig: fontsInFlash=%d, flashInitialized=%d\n", fontsInFlash, flashInitialized);
    if (!storageExists(CONFIG_FILE)) {
        Serial.println("loadConfig: config file not found");
        return false;
    }

    File f = openFromStorage(CONFIG_FILE, FILE_READ);
    if (!f) {
        Serial.println("loadConfig: failed to open config file");
        return false;
    }

    String line = f.readStringUntil('\n'); line.trim();
    config.wakeIntervalMinutes = line.toInt();
    if (config.wakeIntervalMinutes != 1 && config.wakeIntervalMinutes != 2 &&
        config.wakeIntervalMinutes != 5 && config.wakeIntervalMinutes != 10 &&
        config.wakeIntervalMinutes != 15) {
        f.close(); return false;
    }

    line = f.readStringUntil('\n'); line.trim();
    config.allowDifferentFont = (line == "1");
    line = f.readStringUntil('\n'); line.trim();
    config.allowDifferentMode = (line == "1");
    line = f.readStringUntil('\n'); line.trim();
    config.isDebugMode = (line == "1");
    line = f.readStringUntil('\n'); line.trim();
    config.flipInterface = (line == "1");

    // Font flags until "---"
    int fi = 0;
    while (f.available()) {
        line = f.readStringUntil('\n'); line.trim();
        if (line == "---") break;
        if (fi < MAX_FONTS) config.fontEnabled[fi++] = (line == "1");
    }

    // Range flags
    int ri = 0;
    while (f.available() && ri < NUM_GLYPH_RANGES) {
        line = f.readStringUntil('\n'); line.trim();
        if (line.length() > 0) config.rangeEnabled[ri++] = (line == "1");
    }

    f.close();
    Serial.printf("Config loaded: %dmin, diffFont=%d, diffMode=%d\n",
                  config.wakeIntervalMinutes, config.allowDifferentFont, config.allowDifferentMode);
    return true;
}

bool saveConfig() {
    M5.Display.waitDisplay();

    File f = openFromStorage(CONFIG_FILE, FILE_WRITE);
    if (!f) { Serial.println("ERROR: Can't write config"); return false; }

    f.println(config.wakeIntervalMinutes);
    f.println(config.allowDifferentFont ? "1" : "0");
    f.println(config.allowDifferentMode ? "1" : "0");
    f.println(config.isDebugMode ? "1" : "0");
    f.println(config.flipInterface ? "1" : "0");

    for (int i = 0; i < fontCount; i++)
        f.println(config.fontEnabled[i] ? "1" : "0");

    f.println("---");
    for (int i = 0; i < NUM_GLYPH_RANGES; i++)
        f.println(config.rangeEnabled[i] ? "1" : "0");

    f.close();

    // Also save debug flag to NVS so early boot can check without SD
    {
        Preferences dbgPrefs;
        dbgPrefs.begin("ps3cfg", false);
        dbgPrefs.putBool("debug", config.isDebugMode);
        dbgPrefs.end();
    }

    Serial.println("Config saved");
    return true;
}

// Refresh management (same rules as original PaperSpecimen)
static const uint8_t MAX_PARTIAL_BEFORE_FULL = 5;
static const unsigned long FULL_REFRESH_TIMEOUT_MS = 10000; // 10 seconds
static uint8_t partialRefreshCount = 0;
static unsigned long firstPartialAfterFullTime = 0;
static bool hasPartialSinceLastFull = false;
static unsigned long lastFullRefreshTime = 0;
static bool isFirstRender = true;

// FreeType
static FT_Library ftLibrary = nullptr;
static FT_Face ftFace = nullptr;

// FT_Open_Face stream: reads directly from file (SD or LittleFS)
static File fontFile;           // persistent file handle, open while font is in use
static FT_StreamRec fontStream; // FreeType stream descriptor

// FreeType stream callback: read bytes from file at given offset
static unsigned long ft_stream_io(FT_Stream stream, unsigned long offset,
                                   unsigned char* buffer, unsigned long count) {
    File* f = (File*)stream->descriptor.pointer;
    if (!f || !*f) return 0;
    f->seek(offset);
    if (count == 0) return 0; // seek-only call
    return f->read(buffer, count);
}

// FreeType stream callback: close file
static void ft_stream_close(FT_Stream stream) {
    File* f = (File*)stream->descriptor.pointer;
    if (f && *f) {
        f->close();
    }
}

// Display
static int displayW, displayH;

// View mode
enum ViewMode { VIEW_BITMAP, VIEW_OUTLINE };
static ViewMode currentViewMode = VIEW_BITMAP;

// Debug mode (activated by 4+ taps during splash)
static bool debugMode = false;
static float batteryPct = 100.0;  // updated at every boot

// ---- Outline data structures ----

#define MAX_OUTLINE_POINTS 600
#define MAX_OUTLINE_SEGMENTS 600

struct OutlinePoint {
    float x, y;
    bool is_control;
};

enum SegmentType { SEG_MOVE, SEG_LINE, SEG_CONIC, SEG_CUBIC };

struct OutlineSegment {
    SegmentType type;
    float x, y;
    float cx, cy;
    float cx2, cy2;
};

static OutlinePoint g_outline_points[MAX_OUTLINE_POINTS];
static OutlineSegment g_outline_segments[MAX_OUTLINE_SEGMENTS];
static int g_num_points = 0;
static int g_num_segments = 0;

struct OutlineDecomposeContext {
    int segment_count;
    int moveto_count;
    int lineto_count;
    int conicto_count;
    int cubicto_count;
    float scale;
    float offset_x;
    float offset_y;
};

// ---- Font scanning ----

bool isValidFontFile(const char* name) {
    String n = String(name);
    // Skip macOS hidden/resource fork files (._xxx, .DS_Store, etc.)
    if (n.startsWith(".")) return false;
    n.toLowerCase();
    return n.endsWith(".ttf") || n.endsWith(".otf");
}

// Scan fonts from a given filesystem (SD or LittleFS)
int scanFontsFrom(fs::FS &fs, const char* label) {
    File dir = fs.open("/fonts");
    if (!dir || !dir.isDirectory()) {
        Serial.printf("[%s] /fonts directory not found\n", label);
        return 0;
    }

    fontCount = 0;
    File entry;
    while ((entry = dir.openNextFile()) && fontCount < MAX_FONTS) {
        const char* name = entry.name();
        if (!entry.isDirectory() && isValidFontFile(name)) {
            fontPaths[fontCount] = String("/fonts/") + name;
            String displayName = String(name);
            int dotPos = displayName.lastIndexOf('.');
            if (dotPos > 0) displayName = displayName.substring(0, dotPos);
            fontNames[fontCount] = displayName;
            Serial.printf("  [%d] %s (%d bytes)\n", fontCount, name, entry.size());
            fontCount++;
        }
        entry.close();
    }
    dir.close();
    return fontCount;
}

int scanFonts() {
    if (fontsInFlash && flashInitialized) {
        return scanFontsFrom(LittleFS, "Flash");
    } else {
        return scanFontsFrom(SD, "SD");
    }
}

// Calculate total size of fonts on SD
size_t calcSDFontsSize() {
    File dir = SD.open("/fonts");
    if (!dir || !dir.isDirectory()) return 0;
    size_t total = 0;
    File entry;
    while ((entry = dir.openNextFile())) {
        const char* name = entry.name();
        if (!entry.isDirectory() && isValidFontFile(name)) {
            total += entry.size();
        }
        entry.close();
    }
    dir.close();
    return total;
}

// Extract embedded default fonts to flash (first boot without SD)
bool extractEmbeddedFonts() {
    if (!flashInitialized) {
        if (!LittleFS.begin(true)) {
            Serial.println("ERROR: Cannot init LittleFS for embedded fonts");
            return false;
        }
        flashInitialized = true;
    }
    LittleFS.mkdir("/fonts");

    struct EmbeddedFont {
        const unsigned char* data;
        unsigned int size;
        const char* name;
    };

    EmbeddedFont fonts[] = {
        { embedded_apfel_grotezk_bold, embedded_apfel_grotezk_bold_size, embedded_apfel_grotezk_bold_name },
        { embedded_ortica_linear_light, embedded_ortica_linear_light_size, embedded_ortica_linear_light_name },
        { embedded_ronzino_regular, embedded_ronzino_regular_size, embedded_ronzino_regular_name },
    };

    int count = 0;
    for (int i = 0; i < 3; i++) {
        String path = String("/fonts/") + fonts[i].name;
        // Skip if already exists with same size
        File existing = LittleFS.open(path, "r");
        if (existing && existing.size() == fonts[i].size) {
            existing.close();
            Serial.printf("  Skip (exists): %s\n", fonts[i].name);
            continue;
        }
        if (existing) existing.close();

        File f = LittleFS.open(path, "w");
        if (!f) {
            Serial.printf("  ERROR: Cannot write %s\n", fonts[i].name);
            continue;
        }
        // Write from PROGMEM in chunks
        const int chunkSize = 4096;
        unsigned int written = 0;
        while (written < fonts[i].size) {
            int toWrite = min((unsigned int)chunkSize, fonts[i].size - written);
            uint8_t buf[chunkSize];
            memcpy_P(buf, fonts[i].data + written, toWrite);
            f.write(buf, toWrite);
            written += toWrite;
        }
        f.close();
        Serial.printf("  Extracted: %s (%d bytes)\n", fonts[i].name, fonts[i].size);
        count++;
    }
    Serial.printf("Embedded fonts extracted: %d\n", count);
    return true;
}

// Copy all fonts from SD to flash (LittleFS)
bool copyFontsToFlash() {
    if (!flashInitialized) {
        if (!LittleFS.begin(true)) { // format if needed
            Serial.println("LittleFS init failed");
            return false;
        }
        flashInitialized = true;
    }

    // Create /fonts dir on flash
    LittleFS.mkdir("/fonts");

    // First: remove old fonts from flash that no longer exist on SD
    File flashDir = LittleFS.open("/fonts");
    if (flashDir && flashDir.isDirectory()) {
        File fe;
        while ((fe = flashDir.openNextFile())) {
            String flashName = String(fe.name());
            fe.close();
            // Check if this font still exists on SD
            String sdPath = "/fonts/" + flashName;
            File sdCheck = SD.open(sdPath);
            if (!sdCheck) {
                String delPath = "/fonts/" + flashName;
                LittleFS.remove(delPath);
                Serial.printf("  Removed from flash: %s\n", flashName.c_str());
            } else {
                sdCheck.close();
            }
        }
        flashDir.close();
    }

    // Copy fonts from SD to flash
    File dir = SD.open("/fonts");
    if (!dir || !dir.isDirectory()) return false;

    File entry;
    int copied = 0;
    while ((entry = dir.openNextFile())) {
        const char* name = entry.name();
        if (!entry.isDirectory() && isValidFontFile(name)) {
            String destPath = String("/fonts/") + name;
            size_t fileSize = entry.size();

            // Read SD file into PSRAM (needed for both comparison and copy)
            uint8_t* buf = (uint8_t*)ps_malloc(fileSize);
            if (!buf) {
                Serial.printf("  PSRAM alloc failed for %s (%d bytes)\n", name, fileSize);
                entry.close();
                continue;
            }
            entry.read(buf, fileSize);
            entry.close();

            // Compare with existing flash file: same size + same first/last 64 bytes
            File existing = LittleFS.open(destPath, FILE_READ);
            if (existing && existing.size() == fileSize && fileSize > 0) {
                bool same = true;
                // Check first 64 bytes
                int checkLen = (fileSize < 64) ? fileSize : 64;
                uint8_t cmpBuf[64];
                existing.read(cmpBuf, checkLen);
                if (memcmp(buf, cmpBuf, checkLen) != 0) same = false;
                // Check last 64 bytes (if file is large enough)
                if (same && fileSize > 128) {
                    existing.seek(fileSize - 64);
                    existing.read(cmpBuf, 64);
                    if (memcmp(buf + fileSize - 64, cmpBuf, 64) != 0) same = false;
                }
                existing.close();
                if (same) {
                    free(buf);
                    Serial.printf("  Skip (unchanged): %s\n", name);
                    continue;
                }
            }
            if (existing) existing.close();

            File dest = LittleFS.open(destPath, FILE_WRITE);
            if (dest) {
                dest.write(buf, fileSize);
                dest.close();
                copied++;
                Serial.printf("  Copied to flash: %s (%d bytes)\n", name, fileSize);
            } else {
                Serial.printf("  Failed to write: %s\n", name);
            }
            free(buf);
        } else {
            entry.close();
        }
    }
    dir.close();
    Serial.printf("Flash copy done: %d fonts copied\n", copied);
    return true;
}

// Copy config from SD to flash
void copyConfigToFlash() {
    File src = SD.open(CONFIG_FILE, FILE_READ);
    if (!src) return;
    size_t sz = src.size();
    uint8_t* buf = (uint8_t*)ps_malloc(sz);
    if (buf) {
        src.read(buf, sz);
        src.close();
        File dst = LittleFS.open(CONFIG_FILE, FILE_WRITE);
        if (dst) {
            dst.write(buf, sz);
            dst.close();
            Serial.println("Config copied to flash");
        }
        free(buf);
    } else {
        src.close();
    }
}

// ---- FreeType ----

bool loadFontFromStream(int index) {
    // Close previous face and file
    if (ftFace) { FT_Done_Face(ftFace); ftFace = nullptr; }
    if (fontFile) { fontFile.close(); }

    // Wait for any pending display refresh before accessing storage
    M5.Display.waitDisplay();

    if (fontsInFlash && flashInitialized) {
        fontFile = LittleFS.open(fontPaths[index], FILE_READ);
    } else {
        fontFile = SD.open(fontPaths[index], FILE_READ);
    }
    if (!fontFile) {
        Serial.printf("Failed to open: %s\n", fontPaths[index].c_str());
        return false;
    }

    size_t fileSize = fontFile.size();

    // Set up FreeType stream to read directly from file (no RAM copy)
    memset(&fontStream, 0, sizeof(fontStream));
    fontStream.base = nullptr;
    fontStream.size = fileSize;
    fontStream.pos = 0;
    fontStream.descriptor.pointer = &fontFile;
    fontStream.read = ft_stream_io;
    fontStream.close = nullptr; // we manage the file ourselves

    FT_Open_Args openArgs;
    memset(&openArgs, 0, sizeof(openArgs));
    openArgs.flags = FT_OPEN_STREAM;
    openArgs.stream = &fontStream;

    FT_Error err = FT_Open_Face(ftLibrary, &openArgs, 0, &ftFace);
    if (err) {
        Serial.printf("FT_Open_Face failed: 0x%02X\n", err);
        fontFile.close();
        return false;
    }

    Serial.printf("Loaded: %s %s (%ld glyphs)\n",
                  ftFace->family_name, ftFace->style_name, ftFace->num_glyphs);
    return true;
}

// Calculate optimal pixel size so glyph fills TARGET_GLYPH_SIZE
// Same as original: FT_LOAD_NO_SCALE for raw font units
int calculatePixelSize(uint32_t charcode) {
    if (!ftFace) return TARGET_GLYPH_SIZE;

    FT_UInt idx = FT_Get_Char_Index(ftFace, charcode);
    if (idx == 0) return TARGET_GLYPH_SIZE;

    FT_Error err = FT_Load_Glyph(ftFace, idx, FT_LOAD_NO_SCALE);
    if (err) return TARGET_GLYPH_SIZE;

    FT_BBox bbox;
    FT_Outline_Get_CBox(&ftFace->glyph->outline, &bbox);

    float width = bbox.xMax - bbox.xMin;
    float height = bbox.yMax - bbox.yMin;
    float maxDim = (width > height) ? width : height;

    if (maxDim == 0) return TARGET_GLYPH_SIZE;

    int pixelSize = (int)((TARGET_GLYPH_SIZE * (float)ftFace->units_per_EM) / maxDim);
    if (pixelSize < 1) pixelSize = 1;
    if (pixelSize > 2000) pixelSize = 2000;

    Serial.printf("Glyph bbox: w=%.0f h=%.0f, upm=%d, pixel_size=%d\n",
                  width, height, ftFace->units_per_EM, pixelSize);
    return pixelSize;
}

uint32_t findRandomGlyph() {
    // Build list of enabled range indices
    int enabledRanges[NUM_GLYPH_RANGES];
    int numEnabled = 0;
    for (int i = 0; i < NUM_GLYPH_RANGES; i++) {
        if (config.rangeEnabled[i]) enabledRanges[numEnabled++] = i;
    }
    // Fallback: first 6 if none enabled
    if (numEnabled == 0) {
        for (int i = 0; i < 6 && i < NUM_GLYPH_RANGES; i++)
            enabledRanges[numEnabled++] = i;
    }

    for (int attempt = 0; attempt < 100; attempt++) {
        int ri = enabledRanges[random(numEnabled)];
        uint32_t cp = random(glyphRanges[ri].start, glyphRanges[ri].end + 1);
        FT_UInt idx = FT_Get_Char_Index(ftFace, cp);
        if (idx != 0) return cp;
    }
    return 'A';
}

// ---- FreeType label rendering ----

// Measure string width in pixels using FreeType at given pixel size
int ftStringWidth(const char* text, int pixSize) {
    if (!ftFace) return 0;
    FT_Set_Pixel_Sizes(ftFace, 0, pixSize);
    int totalWidth = 0;
    for (const char* p = text; *p; p++) {
        FT_UInt idx = FT_Get_Char_Index(ftFace, (uint8_t)*p);
        if (idx == 0) idx = FT_Get_Char_Index(ftFace, '?');
        if (idx == 0) continue;
        FT_Load_Glyph(ftFace, idx, FT_LOAD_DEFAULT);
        totalWidth += ftFace->glyph->advance.x >> 6;
    }
    return totalWidth;
}

// Shorten text for FreeType rendering: keeps start + "..." + end, measured in FT pixels
String shortenFTText(const String& text, int maxWidth, int pixSize) {
    if (ftStringWidth(text.c_str(), pixSize) <= maxWidth) return text;

    int textLen = text.length();
    int best = 1;
    for (int chars = textLen / 2; chars >= 1; chars--) {
        String shortened = text.substring(0, chars) + "..." + text.substring(textLen - chars);
        if (ftStringWidth(shortened.c_str(), pixSize) <= maxWidth) {
            best = chars;
            break;
        }
    }
    return text.substring(0, best) + "..." + text.substring(textLen - best);
}

// Render a string using FreeType at given pixel size, centered at (cx, y)
// Returns the height used. Uses current ftFace.
void drawFTString(const char* text, int cx, int y, int pixSize, bool bottomAlign) {
    if (!ftFace) return;

    FT_Set_Pixel_Sizes(ftFace, 0, pixSize);

    // First pass: measure total width and max ascent/descent
    // Use FT_LOAD_RENDER directly — gives both advance and bitmap metrics in one call
    int totalWidth = 0;
    int maxAscent = 0;
    int maxDescent = 0;
    for (const char* p = text; *p; p++) {
        FT_UInt idx = FT_Get_Char_Index(ftFace, (uint8_t)*p);
        if (idx == 0) idx = FT_Get_Char_Index(ftFace, '?');
        if (idx == 0) continue;

        FT_Load_Glyph(ftFace, idx, FT_LOAD_RENDER);
        totalWidth += ftFace->glyph->advance.x >> 6;
        int asc = ftFace->glyph->bitmap_top;
        int desc = (int)ftFace->glyph->bitmap.rows - ftFace->glyph->bitmap_top;
        if (asc > maxAscent) maxAscent = asc;
        if (desc > maxDescent) maxDescent = desc;
    }

    // Calculate start position
    int startX = cx - totalWidth / 2;
    int baselineY;
    if (bottomAlign) {
        baselineY = y - maxDescent;
    } else {
        baselineY = y + maxAscent;
    }

    // Second pass: render using pushGrayscaleImage per character
    {
        int penX = startX;
        for (const char* p = text; *p; p++) {
            FT_UInt idx = FT_Get_Char_Index(ftFace, (uint8_t)*p);
            if (idx == 0) idx = FT_Get_Char_Index(ftFace, '?');
            if (idx == 0) { penX += pixSize / 2; continue; }

            FT_Load_Glyph(ftFace, idx, FT_LOAD_RENDER);
            FT_GlyphSlot slot = ftFace->glyph;
            FT_Bitmap* bmp = &slot->bitmap;
            int bx = penX + slot->bitmap_left;
            int by = baselineY - slot->bitmap_top;

            if (bmp->width > 0 && bmp->rows > 0) {
                // Clamp to display bounds
                int sx = 0, sy = 0;
                int dx = bx, dy = by;
                int w = (int)bmp->width, h = (int)bmp->rows;
                if (dx < 0) { sx = -dx; w += dx; dx = 0; }
                if (dy < 0) { sy = -dy; h += dy; dy = 0; }
                if (dx + w > displayW) w = displayW - dx;
                if (dy + h > displayH) h = displayH - dy;

                if (w > 0 && h > 0) {
                    if (sx == 0 && sy == 0 && w == (int)bmp->width && bmp->pitch == (int)bmp->width) {
                        M5.Display.pushGrayscaleImage(dx, dy, w, h,
                            bmp->buffer, lgfx::color_depth_t::grayscale_8bit, TFT_BLACK, TFT_WHITE);
                    } else {
                        // Copy clipped region into contiguous buffer
                        uint8_t* buf = (uint8_t*)malloc(w * h);
                        if (buf) {
                            for (int row = 0; row < h; row++) {
                                memcpy(&buf[row * w],
                                       &bmp->buffer[(sy + row) * bmp->pitch + sx], w);
                            }
                            M5.Display.pushGrayscaleImage(dx, dy, w, h,
                                buf, lgfx::color_depth_t::grayscale_8bit, TFT_BLACK, TFT_WHITE);
                            free(buf);
                        }
                    }
                }
            }
            penX += slot->advance.x >> 6;
        }
    }
}

// Forward declaration
void refreshDisplay();

// Check if a glyph has visible content (non-blank bitmap)
// Returns true if the glyph exists AND has non-zero bitmap dimensions
bool isGlyphVisible(uint32_t charcode) {
    if (!ftFace) return false;
    FT_UInt idx = FT_Get_Char_Index(ftFace, charcode);
    if (idx == 0) return false;
    // Load outline only (no rasterization) — check if glyph has contour points
    FT_Error err = FT_Load_Glyph(ftFace, idx, FT_LOAD_NO_SCALE);
    if (err) return false;
    return (ftFace->glyph->outline.n_points > 0);
}

// ---- Outline decompose callbacks ----

int outlineMoveTo(const FT_Vector* to, void* user) {
    OutlineDecomposeContext* ctx = (OutlineDecomposeContext*)user;
    ctx->segment_count++;
    ctx->moveto_count++;

    if (g_num_segments < MAX_OUTLINE_SEGMENTS) {
        g_outline_segments[g_num_segments].type = SEG_MOVE;
        g_outline_segments[g_num_segments].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_segments[g_num_segments].y = -to->y * ctx->scale + ctx->offset_y;
        g_num_segments++;
    }
    if (g_num_points < MAX_OUTLINE_POINTS) {
        g_outline_points[g_num_points].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_points[g_num_points].y = -to->y * ctx->scale + ctx->offset_y;
        g_outline_points[g_num_points].is_control = false;
        g_num_points++;
    }
    return 0;
}

int outlineLineTo(const FT_Vector* to, void* user) {
    OutlineDecomposeContext* ctx = (OutlineDecomposeContext*)user;
    ctx->segment_count++;
    ctx->lineto_count++;

    if (g_num_segments < MAX_OUTLINE_SEGMENTS) {
        g_outline_segments[g_num_segments].type = SEG_LINE;
        g_outline_segments[g_num_segments].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_segments[g_num_segments].y = -to->y * ctx->scale + ctx->offset_y;
        g_num_segments++;
    }
    if (g_num_points < MAX_OUTLINE_POINTS) {
        g_outline_points[g_num_points].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_points[g_num_points].y = -to->y * ctx->scale + ctx->offset_y;
        g_outline_points[g_num_points].is_control = false;
        g_num_points++;
    }
    return 0;
}

int outlineConicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
    OutlineDecomposeContext* ctx = (OutlineDecomposeContext*)user;
    ctx->segment_count++;
    ctx->conicto_count++;

    if (g_num_segments < MAX_OUTLINE_SEGMENTS) {
        g_outline_segments[g_num_segments].type = SEG_CONIC;
        g_outline_segments[g_num_segments].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_segments[g_num_segments].y = -to->y * ctx->scale + ctx->offset_y;
        g_outline_segments[g_num_segments].cx = control->x * ctx->scale + ctx->offset_x;
        g_outline_segments[g_num_segments].cy = -control->y * ctx->scale + ctx->offset_y;
        g_num_segments++;
    }
    if (g_num_points < MAX_OUTLINE_POINTS) {
        g_outline_points[g_num_points].x = control->x * ctx->scale + ctx->offset_x;
        g_outline_points[g_num_points].y = -control->y * ctx->scale + ctx->offset_y;
        g_outline_points[g_num_points].is_control = true;
        g_num_points++;
    }
    if (g_num_points < MAX_OUTLINE_POINTS) {
        g_outline_points[g_num_points].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_points[g_num_points].y = -to->y * ctx->scale + ctx->offset_y;
        g_outline_points[g_num_points].is_control = false;
        g_num_points++;
    }
    return 0;
}

int outlineCubicTo(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
    OutlineDecomposeContext* ctx = (OutlineDecomposeContext*)user;
    ctx->segment_count++;
    ctx->cubicto_count++;

    if (g_num_segments < MAX_OUTLINE_SEGMENTS) {
        g_outline_segments[g_num_segments].type = SEG_CUBIC;
        g_outline_segments[g_num_segments].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_segments[g_num_segments].y = -to->y * ctx->scale + ctx->offset_y;
        g_outline_segments[g_num_segments].cx = control1->x * ctx->scale + ctx->offset_x;
        g_outline_segments[g_num_segments].cy = -control1->y * ctx->scale + ctx->offset_y;
        g_outline_segments[g_num_segments].cx2 = control2->x * ctx->scale + ctx->offset_x;
        g_outline_segments[g_num_segments].cy2 = -control2->y * ctx->scale + ctx->offset_y;
        g_num_segments++;
    }
    if (g_num_points < MAX_OUTLINE_POINTS) {
        g_outline_points[g_num_points].x = control1->x * ctx->scale + ctx->offset_x;
        g_outline_points[g_num_points].y = -control1->y * ctx->scale + ctx->offset_y;
        g_outline_points[g_num_points].is_control = true;
        g_num_points++;
    }
    if (g_num_points < MAX_OUTLINE_POINTS) {
        g_outline_points[g_num_points].x = control2->x * ctx->scale + ctx->offset_x;
        g_outline_points[g_num_points].y = -control2->y * ctx->scale + ctx->offset_y;
        g_outline_points[g_num_points].is_control = true;
        g_num_points++;
    }
    if (g_num_points < MAX_OUTLINE_POINTS) {
        g_outline_points[g_num_points].x = to->x * ctx->scale + ctx->offset_x;
        g_outline_points[g_num_points].y = -to->y * ctx->scale + ctx->offset_y;
        g_outline_points[g_num_points].is_control = false;
        g_num_points++;
    }
    return 0;
}

// ---- Outline parsing ----

bool parseGlyphOutline(uint32_t codepoint) {
    g_num_segments = 0;
    g_num_points = 0;

    if (!ftFace) return false;

    FT_UInt glyphIndex = FT_Get_Char_Index(ftFace, codepoint);
    if (glyphIndex == 0) return false;

    FT_Error err = FT_Load_Glyph(ftFace, glyphIndex, FT_LOAD_NO_SCALE);
    if (err) return false;

    if (ftFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;

    FT_Outline* outline = &ftFace->glyph->outline;

    FT_BBox bbox;
    FT_Outline_Get_CBox(outline, &bbox);

    float width = bbox.xMax - bbox.xMin;
    float height = bbox.yMax - bbox.yMin;
    float maxDim = (width > height) ? width : height;
    if (maxDim == 0) return false;

    float target_size = (float)TARGET_GLYPH_SIZE;
    float scale = target_size / maxDim;

    int centerX = displayW / 2;
    int centerY = displayH / 2;

    float bbox_center_x = (bbox.xMin + bbox.xMax) / 2.0f;
    float bbox_center_y = (bbox.yMin + bbox.yMax) / 2.0f;

    float offset_x = centerX - bbox_center_x * scale;
    float offset_y = centerY + bbox_center_y * scale;

    OutlineDecomposeContext ctx = {0, 0, 0, 0, 0, scale, offset_x, offset_y};

    FT_Outline_Funcs callbacks;
    callbacks.move_to = (FT_Outline_MoveToFunc)outlineMoveTo;
    callbacks.line_to = (FT_Outline_LineToFunc)outlineLineTo;
    callbacks.conic_to = (FT_Outline_ConicToFunc)outlineConicTo;
    callbacks.cubic_to = (FT_Outline_CubicToFunc)outlineCubicTo;
    callbacks.shift = 0;
    callbacks.delta = 0;

    FT_Error decompose_err = FT_Outline_Decompose(outline, &callbacks, &ctx);
    if (decompose_err) return false;

    Serial.printf("Outline: %d segs (M:%d L:%d Q:%d C:%d), %d pts\n",
                  g_num_segments, ctx.moveto_count, ctx.lineto_count,
                  ctx.conicto_count, ctx.cubicto_count, g_num_points);
    return true;
}

// ---- Dashed line helper ----

// Colors for outline mode (M5GFX color565: 0,0,0 = black, 255,255,255 = white)
// Original: 12 = dark gray outline, 15 = black for construction/points
// PaperS3 equivalents:
static uint16_t colorOutline;   // dark gray for curves
static uint16_t colorConstruct; // black for construction lines and points

void drawDashedLine(float x1, float y1, float x2, float y2, uint16_t color) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < 0.5f) return;

    dx /= length;
    dy /= length;

    float dash_length = 10.0f;
    float gap_length = 5.0f;
    float distance = 0.0f;
    bool drawing = true;

    while (distance < length) {
        float seg_len = drawing ? dash_length : gap_length;
        float end_dist = distance + seg_len;
        if (end_dist > length) end_dist = length;

        if (drawing) {
            M5.Display.drawLine(
                (int)(x1 + dx * distance), (int)(y1 + dy * distance),
                (int)(x1 + dx * end_dist), (int)(y1 + dy * end_dist), color);
        }

        distance = end_dist;
        drawing = !drawing;
    }
}

// Flag to track if display is sleeping (only in glyph mode, not setup)
static bool displaySleeping = false;

// Wake the display controller if it was sleeping
void wakeDisplay() {
    if (displaySleeping) {
        M5.Display.wakeup();
        displaySleeping = false;
    }
}

// Put the display controller to sleep (image stays visible, saves ~5-10mA)
void sleepDisplay() {
    M5.Display.waitDisplay();
    M5.Display.sleep();
    displaySleeping = true;
}

// Delayed display sleep: sleep 3s after the last FULL refresh only
#define DISPLAY_SLEEP_DELAY_MS 3000
static unsigned long lastFullRefreshDone = 0;

// ---- Shared label drawing (font name top, codepoint bottom) ----

void drawLabels(uint32_t charcode) {
    int labelMaxW = displayW - 2 * UI_PAD;
    String displayName = shortenFTText(fontNames[currentFontIndex], labelMaxW, LABEL_PIXEL_SIZE);
    drawFTString(displayName.c_str(),
                 displayW / 2, UI_PAD, LABEL_PIXEL_SIZE, false);

    char cpBuf[32];
    if (debugMode) {
        snprintf(cpBuf, sizeof(cpBuf), "U+%04X - %.0f%%", charcode, batteryPct);
    } else {
        snprintf(cpBuf, sizeof(cpBuf), "U+%04X", charcode);
    }
    drawFTString(cpBuf, displayW / 2, displayH - UI_PAD, LABEL_PIXEL_SIZE, true);
}

// ---- Outline rendering ----

void drawGlyphOutline(uint32_t charcode) {
    if (!ftFace) return;
    wakeDisplay();

    FT_UInt glyphIndex = FT_Get_Char_Index(ftFace, charcode);
    if (glyphIndex == 0) {
        charcode = findRandomGlyph();
        glyphIndex = FT_Get_Char_Index(ftFace, charcode);
    }

    if (!parseGlyphOutline(charcode)) {
        Serial.printf("Failed to parse outline for U+%04X\n", charcode);
        return;
    }

    M5.Display.fillScreen(TFT_WHITE);

    // Draw outline curve segments (dark gray)
    float curr_x = 0, curr_y = 0;
    for (int i = 0; i < g_num_segments; i++) {
        OutlineSegment& seg = g_outline_segments[i];
        switch (seg.type) {
            case SEG_MOVE:
                curr_x = seg.x;
                curr_y = seg.y;
                break;
            case SEG_LINE:
                M5.Display.drawLine((int)curr_x, (int)curr_y, (int)seg.x, (int)seg.y, colorOutline);
                curr_x = seg.x;
                curr_y = seg.y;
                break;
            case SEG_CONIC: {
                float px = curr_x, py = curr_y;
                for (int t = 1; t <= 10; t++) {
                    float u = t / 10.0f;
                    float u1 = 1.0f - u;
                    float bx = u1*u1*curr_x + 2*u1*u*seg.cx + u*u*seg.x;
                    float by = u1*u1*curr_y + 2*u1*u*seg.cy + u*u*seg.y;
                    M5.Display.drawLine((int)px, (int)py, (int)bx, (int)by, colorOutline);
                    px = bx; py = by;
                }
                curr_x = seg.x; curr_y = seg.y;
                break;
            }
            case SEG_CUBIC: {
                float px = curr_x, py = curr_y;
                for (int t = 1; t <= 15; t++) {
                    float u = t / 15.0f;
                    float u1 = 1.0f - u;
                    float bx = u1*u1*u1*curr_x + 3*u1*u1*u*seg.cx + 3*u1*u*u*seg.cx2 + u*u*u*seg.x;
                    float by = u1*u1*u1*curr_y + 3*u1*u1*u*seg.cy + 3*u1*u*u*seg.cy2 + u*u*u*seg.y;
                    M5.Display.drawLine((int)px, (int)py, (int)bx, (int)by, colorOutline);
                    px = bx; py = by;
                }
                curr_x = seg.x; curr_y = seg.y;
                break;
            }
        }
    }

    // Draw construction dashed lines (black)
    for (int i = 0; i < g_num_segments; i++) {
        OutlineSegment& seg = g_outline_segments[i];
        float start_x = 0, start_y = 0;
        if (i > 0) {
            start_x = g_outline_segments[i-1].x;
            start_y = g_outline_segments[i-1].y;
        }

        if (seg.type == SEG_CONIC) {
            drawDashedLine(start_x, start_y, seg.cx, seg.cy, colorConstruct);
            drawDashedLine(seg.cx, seg.cy, seg.x, seg.y, colorConstruct);
        } else if (seg.type == SEG_CUBIC) {
            drawDashedLine(start_x, start_y, seg.cx, seg.cy, colorConstruct);
            drawDashedLine(seg.cx, seg.cy, seg.cx2, seg.cy2, colorConstruct);
            drawDashedLine(seg.cx2, seg.cy2, seg.x, seg.y, colorConstruct);
        }
    }

    // Draw points (10px diameter, anti-aliased)
    for (int i = 0; i < g_num_points; i++) {
        OutlinePoint& pt = g_outline_points[i];
        if (pt.is_control) {
            // Off-curve: hollow circle (anti-aliased outer, white inner)
            M5.Display.fillSmoothCircle((int)pt.x, (int)pt.y, 4, colorConstruct);
            M5.Display.fillSmoothCircle((int)pt.x, (int)pt.y, 3, TFT_WHITE);
        } else {
            // On-curve: filled circle (anti-aliased)
            M5.Display.fillSmoothCircle((int)pt.x, (int)pt.y, 4, colorConstruct);
        }
    }

    drawLabels(charcode);
    refreshDisplay();
    currentCodepoint = charcode;
}

// ---- Refresh management (same logic as original PaperSpecimen) ----

void refreshDisplay() {
    if (isFirstRender) {
        // First render: full quality refresh
        Serial.println("First render: full refresh");
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.display();
        isFirstRender = false;
        partialRefreshCount = 0;
        hasPartialSinceLastFull = false;
        lastFullRefreshTime = millis();
        lastFullRefreshDone = millis();
        return;
    }

    // Check if full refresh is needed BEFORE displaying
    // (dirty rect is consumed by display(), so we must decide now)
    bool needFull = false;

    if (hasPartialSinceLastFull) {
        unsigned long timeSinceFirstPartial = millis() - firstPartialAfterFullTime;
        unsigned long timeSinceLastFull = millis() - lastFullRefreshTime;

        if ((partialRefreshCount >= MAX_PARTIAL_BEFORE_FULL ||
             timeSinceFirstPartial >= FULL_REFRESH_TIMEOUT_MS) &&
            timeSinceLastFull >= FULL_REFRESH_TIMEOUT_MS) {
            needFull = true;
        }
    }

    if (needFull) {
        Serial.printf("Full refresh (count=%d)\n", partialRefreshCount);
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.display();
        partialRefreshCount = 0;
        hasPartialSinceLastFull = false;
        lastFullRefreshTime = millis();
        lastFullRefreshDone = millis();
    } else {
        // Partial refresh with epd_text (preserves 16-level grayscale anti-aliasing)
        // epd_fast/epd_fastest reduce to black/white only - no good for font rendering
        M5.Display.setEpdMode(epd_mode_t::epd_text);
        M5.Display.display();

        if (!hasPartialSinceLastFull) {
            firstPartialAfterFullTime = millis();
            hasPartialSinceLastFull = true;
            Serial.println("First partial after full - starting 10s timer");
        }
        partialRefreshCount++;
        Serial.printf("Partial refresh #%d\n", partialRefreshCount);
    }
}

// ---- Main glyph drawing ----

void drawGlyph(uint32_t charcode) {
    if (!ftFace) return;
    wakeDisplay();

    FT_UInt glyphIndex = FT_Get_Char_Index(ftFace, charcode);
    if (glyphIndex == 0) {
        Serial.printf("Glyph U+%04X not in font, finding alternative\n", charcode);
        charcode = findRandomGlyph();
        glyphIndex = FT_Get_Char_Index(ftFace, charcode);
    }

    // Calculate per-glyph pixel size
    int pixelSize = calculatePixelSize(charcode);
    Serial.printf("Rendering U+%04X at %dpx\n", charcode, pixelSize);

    FT_Set_Pixel_Sizes(ftFace, 0, pixelSize);
    FT_Error err = FT_Load_Glyph(ftFace, glyphIndex, FT_LOAD_RENDER);
    if (err) {
        Serial.printf("FT_Load_Glyph failed: 0x%02X\n", err);
        return;
    }

    FT_GlyphSlot slot = ftFace->glyph;
    FT_Bitmap* bmp = &slot->bitmap;

    Serial.printf("Bitmap: %dx%d, bearing(%d, %d)\n",
                  bmp->width, bmp->rows, slot->bitmap_left, slot->bitmap_top);

    // Clear display
    M5.Display.fillScreen(TFT_WHITE);

    // Center bitmap on screen
    int drawX = (displayW - (int)bmp->width) / 2;
    int drawY = (displayH - (int)bmp->rows) / 2;

    // Clamp to display bounds
    int srcX = 0, srcY = 0;
    int w = (int)bmp->width, h = (int)bmp->rows;
    if (drawX < 0) { srcX = -drawX; w += drawX; drawX = 0; }
    if (drawY < 0) { srcY = -drawY; h += drawY; drawY = 0; }
    if (drawX + w > displayW) w = displayW - drawX;
    if (drawY + h > displayH) h = displayH - drawY;

    if (w > 0 && h > 0) {
        // Use pushGrayscaleImage for efficient bulk rendering
        // FreeType bitmap is 8-bit alpha (0=transparent, 255=opaque)
        // pushGrayscaleImage maps values between forecolor (black) and backcolor (white)
        // We need a contiguous w*h buffer (no pitch gaps, no clipping offsets)
        if (srcX == 0 && srcY == 0 && w == (int)bmp->width && bmp->pitch == (int)bmp->width) {
            // Perfect case: buffer is already contiguous
            M5.Display.pushGrayscaleImage(drawX, drawY, w, h,
                bmp->buffer, lgfx::color_depth_t::grayscale_8bit, TFT_BLACK, TFT_WHITE);
        } else {
            // Need to copy into contiguous buffer (pitch != width, or clipping active)
            uint8_t* buf = (uint8_t*)ps_malloc(w * h);
            if (buf) {
                for (int row = 0; row < h; row++) {
                    memcpy(&buf[row * w],
                           &bmp->buffer[(srcY + row) * bmp->pitch + srcX], w);
                }
                M5.Display.pushGrayscaleImage(drawX, drawY, w, h,
                    buf, lgfx::color_depth_t::grayscale_8bit, TFT_BLACK, TFT_WHITE);
                free(buf);
            } else {
                // Fallback to pixel-by-pixel if PSRAM allocation fails
                for (int row = 0; row < h; row++) {
                    for (int col = 0; col < w; col++) {
                        uint8_t alpha = bmp->buffer[(srcY + row) * bmp->pitch + (srcX + col)];
                        if (alpha > 0) {
                            uint8_t gray = 255 - alpha;
                            M5.Display.drawPixel(drawX + col, drawY + row,
                                M5.Display.color565(gray, gray, gray));
                        }
                    }
                }
            }
        }
    }

    drawLabels(charcode);

    // Refresh with smart management
    refreshDisplay();
    currentCodepoint = charcode;
}

// ---- Setup UI ----

// UI layout (540x960 portrait, single column, Font0 textSize 2)
// 20px uniform spacing between all groups, 20px top/bottom padding
#define UI_LINE_H    38  // line height for items (text is 16px, rest is touch area)
#define UI_LEFT      30

// Setup refresh tracking (reuses same constants as main refresh logic)
static bool setupFirstRender = true;
static int setupPartialCount = 0;
static unsigned long setupLastFullTime = 0;
static unsigned long setupFirstPartialTime = 0;
static bool setupHasPartial = false;

// Apply display rotation based on flip config
void applyRotation() {
    M5.Display.setRotation(config.flipInterface ? 2 : 0);
}

// Common setup for all UI screens — clears and sets font, but no title/version
void uiBeginScreen() {
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextDatum(top_left);
}


void uiFlush() {
    if (setupFirstRender) {
        // First render: always full quality
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        setupFirstRender = false;
        setupPartialCount = 0;
        setupHasPartial = false;
        setupLastFullTime = millis();
    } else {
        // Same refresh rules as main display: full after 5 partials or 10s,
        // but never full within 10s of last full
        bool needFull = false;
        if (setupHasPartial) {
            unsigned long now = millis();
            unsigned long sinceFirstPartial = now - setupFirstPartialTime;
            unsigned long sinceLastFull = now - setupLastFullTime;
            if ((setupPartialCount >= MAX_PARTIAL_BEFORE_FULL ||
                 sinceFirstPartial >= FULL_REFRESH_TIMEOUT_MS) &&
                sinceLastFull >= FULL_REFRESH_TIMEOUT_MS) {
                needFull = true;
            }
        }

        if (needFull) {
            M5.Display.setEpdMode(epd_mode_t::epd_quality);
            setupPartialCount = 0;
            setupHasPartial = false;
            setupLastFullTime = millis();
        } else {
            M5.Display.setEpdMode(epd_mode_t::epd_text);
            if (!setupHasPartial) {
                setupFirstPartialTime = millis();
                setupHasPartial = true;
            }
            setupPartialCount++;
        }
    }
    M5.Display.display();
    M5.Display.waitDisplay();
}

// Helper: measure text width with current font settings
int uiTextWidth(const char* text) {
    return M5.Display.textWidth(text);
}

// Helper: calculate left X to center a group of items,
// where the widest item determines the block width
int uiGroupX(int maxItemWidth) {
    return (displayW - maxItemWidth) / 2;
}

// Shorten text to fit within maxWidth pixels, keeping start and end with "..." in middle
// Same approach as original PaperSpecimen: binary search for optimal chars per side
String shortenText(const String& text, int maxWidth) {
    int textWidth = M5.Display.textWidth(text);
    if (textWidth <= maxWidth) return text;

    int textLen = text.length();
    int ellipsisWidth = M5.Display.textWidth("...");

    // Binary search: find max chars per side that fits
    int best = 1;
    for (int chars = textLen / 2; chars >= 1; chars--) {
        String shortened = text.substring(0, chars) + "..." + text.substring(textLen - chars);
        if (M5.Display.textWidth(shortened.c_str()) <= maxWidth) {
            best = chars;
            break;
        }
    }

    return text.substring(0, best) + "..." + text.substring(textLen - best);
}

// ---- Setup: Main screen ----
// Single column, vertical list. Items:
//   [Confirm]
//   (sep)
//   Refresh timer:
//     (*) 15 min  ( ) 10 min  ( ) 5 min
//   (sep)
//   When in standby:
//     [x] Allow different font
//     [x] Allow different mode
//   (sep)
//   [Unicode ranges >>]
//   (sep)
//   Select/Deselect all fonts
//   (*) FontName1       <<<  page N/M  >>>
//   (*) FontName2
//   ...

// Track touch areas for hit testing
#define MAX_UI_ITEMS 20
struct UiItem {
    int x, y, w, h;
    int id; // action identifier
};
static UiItem uiItems[MAX_UI_ITEMS];
static int uiItemCount = 0;

// Action IDs for main setup
#define ID_CONFIRM       1
#define ID_TIMER_15      10
#define ID_TIMER_10      11
#define ID_TIMER_5       12
#define ID_TIMER_2       13
#define ID_TIMER_1       14
#define ID_DIFF_FONT     20
#define ID_DIFF_MODE     21
#define ID_UNICODE_LINK  30
#define ID_FLIP          31
#define ID_WIFI_MANAGER  32
#define ID_FONT_SELALL   40
#define ID_FONT_BASE     100   // 100 + fontIndex
#define ID_FONT_PREV     200
#define ID_FONT_NEXT     201
#define ID_FONTS_LINK    202   // "> Fonts" link in main setup (>20 fonts)

// Full-width item (touches anywhere on that row)
void addUiItem(int y, int h, int id) {
    if (uiItemCount < MAX_UI_ITEMS) {
        uiItems[uiItemCount++] = {0, y, displayW, h, id}; // full display width
    }
}

// Item with specific X range (for left/right buttons on same row)
void addUiItemX(int x, int y, int w, int h, int id) {
    if (uiItemCount < MAX_UI_ITEMS) {
        uiItems[uiItemCount++] = {x, y, w, h, id};
    }
}

int hitTestUi(int tx, int ty) {
    for (int i = 0; i < uiItemCount; i++) {
        if (tx >= uiItems[i].x && tx < uiItems[i].x + uiItems[i].w &&
            ty >= uiItems[i].y && ty < uiItems[i].y + uiItems[i].h)
            return uiItems[i].id;
    }
    return 0;
}

void renderSetupScreen(int fontPage) {
    uiItemCount = 0;
    char buf[60];

    // -- Pass 1: measure all items to find the widest --
    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::Font0);

    // Indent for labels: align with text after "(*) "
    int indent = uiTextWidth("(*) ");

    int maxW = 0;
    int w;

    // Timer radios: normal 15/10/5, debug 5/2/1
    static const int intervalsNormal[] = {15, 10, 5};
    static const int intervalsDebug[] = {5, 2, 1};
    static const int timerIdsNormal[] = {ID_TIMER_15, ID_TIMER_10, ID_TIMER_5};
    static const int timerIdsDebug[] = {ID_TIMER_5, ID_TIMER_2, ID_TIMER_1};
    const int* intervals = debugMode ? intervalsDebug : intervalsNormal;
    const int* timerIds = debugMode ? timerIdsDebug : timerIdsNormal;
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "(*) %d min", intervals[i]);
        w = uiTextWidth(buf); if (w > maxW) maxW = w;
    }

    // Standby checkboxes
    w = uiTextWidth("(*) Allow different font"); if (w > maxW) maxW = w;
    w = uiTextWidth("(*) Allow different mode"); if (w > maxW) maxW = w;

    // Fonts: if >20 fonts, show as link; otherwise show inline with pagination
    #define FONTS_INLINE_MAX 20
    #define MAX_FONTS_NO_PAGE 5
    #define FONTS_PER_PAGE 4
    bool fontsInline = (fontCount <= FONTS_INLINE_MAX);
    int fontsPerPage = 0, totalPages = 1, startFont = 0, endFont = 0;
    int maxTextW = displayW - 2 * UI_LEFT;

    if (fontsInline) {
        w = uiTextWidth("Fonts:  > Deselect all"); if (w > maxW) maxW = w;
        if (fontCount <= MAX_FONTS_NO_PAGE) {
            fontsPerPage = fontCount;
            totalPages = 1;
        } else {
            fontsPerPage = FONTS_PER_PAGE;
            totalPages = (fontCount + fontsPerPage - 1) / fontsPerPage;
        }
        if (totalPages < 1) totalPages = 1;
        if (fontPage >= totalPages) fontPage = totalPages - 1;

        startFont = fontPage * fontsPerPage;
        endFont = startFont + fontsPerPage;
        if (endFont > fontCount) endFont = fontCount;

        for (int i = startFont; i < endFont; i++) {
            snprintf(buf, sizeof(buf), "(*) %s", fontNames[i].c_str());
            String s = shortenText(String(buf), maxTextW);
            w = uiTextWidth(s.c_str()); if (w > maxW) maxW = w;
        }
    } else {
        w = uiTextWidth("> Fonts"); if (w > maxW) maxW = w;
    }

    // Calculate left X to center the whole block
    int leftX = uiGroupX(maxW);
    if (leftX < UI_LEFT) leftX = UI_LEFT;

    // -- Calculate total content height for vertical centering --
    // Count all lines:
    //   Refresh timer label(1) + 3 radios
    //   + gap
    //   + When in standby label(1) + 2 checkboxes
    //   + gap
    //   + Fonts header(1) + fonts shown + nav line (if paginated)
    //   + gap
    //   + Unicode ranges(1) + Flip interface(1) + Manage fonts WiFi(1)
    //   + gap
    //   + Confirm(1)
    int fontLines;
    if (fontsInline) {
        int fontsShown = endFont - startFont;
        int navLine = (totalPages > 1) ? 1 : 0;
        fontLines = 1 + fontsShown + navLine; // fonts header + fonts + nav
    } else {
        fontLines = 1; // just "> Fonts" link
    }
    int totalLines = 1 + 3          // timer label + 3 radios
                   + 1 + 2          // standby label + 2 checkboxes
                   + fontLines      // fonts (inline or link)
                   + 3              // unicode + flip + wifi manager
                   + 1;             // confirm
    int totalGaps = 4; // after timer radios, after standby, after fonts, before confirm
    int textH = 16; // actual text height at textSize 2, Font0
    // Last line only needs textH, not full UI_LINE_H
    int totalContentH = (totalLines - 1) * UI_LINE_H + textH + totalGaps * UI_PAD;

    // Title at top (fixed), version at bottom (fixed)
    int titleY = UI_PAD;
    int versionY = displayH - UI_PAD;

    // Usable area between title text bottom and version text top
    int areaTop = titleY + textH;
    int areaBot = versionY - textH; // version drawn with bottom_center, so top is textH above
    int areaH = areaBot - areaTop;

    // Center content vertically: equal gap above and below
    int gap = (areaH - totalContentH) / 2;
    if (gap < UI_PAD) gap = UI_PAD;
    int startY = areaTop + gap;

    // -- Pass 2: draw everything --
    uiBeginScreen();
    // Title fixed at top, version fixed at bottom
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("PaperSpecimen S3", displayW / 2, UI_PAD);
    M5.Display.setTextDatum(bottom_center);
    if (debugMode) {
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "%s - %.0f%%", VERSION, batteryPct);
        M5.Display.drawString(vbuf, displayW / 2, displayH - UI_PAD);
    } else {
        M5.Display.drawString(VERSION, displayW / 2, displayH - UI_PAD);
    }
    M5.Display.setTextDatum(top_left);

    int y = startY;

    // Refresh timer (label indented to align with item text)
    M5.Display.drawString("Refresh timer:", leftX + indent, y);
    y += UI_LINE_H;

    for (int i = 0; i < 3; i++) {
        bool sel = (config.wakeIntervalMinutes == intervals[i]);
        snprintf(buf, sizeof(buf), "%s %d min", sel ? "(*)" : "( )", intervals[i]);
        M5.Display.drawString(buf, leftX, y);
        addUiItem(y, UI_LINE_H, timerIds[i]);
        y += UI_LINE_H;
    }
    y += UI_PAD;

    // When in standby (label indented)
    M5.Display.drawString("When in standby:", leftX + indent, y);
    y += UI_LINE_H;

    snprintf(buf, sizeof(buf), "%s Allow different font",
             config.allowDifferentFont ? "(*)" : "( )");
    M5.Display.drawString(buf, leftX, y);
    addUiItem(y, UI_LINE_H, ID_DIFF_FONT);
    y += UI_LINE_H;

    snprintf(buf, sizeof(buf), "%s Allow different mode",
             config.allowDifferentMode ? "(*)" : "( )");
    M5.Display.drawString(buf, leftX, y);
    addUiItem(y, UI_LINE_H, ID_DIFF_MODE);
    y += UI_LINE_H + UI_PAD;

    if (fontsInline) {
        // Fonts header (label indented) with inline list
        bool allFonts = true;
        for (int i = 0; i < fontCount; i++)
            if (!config.fontEnabled[i]) { allFonts = false; break; }
        snprintf(buf, sizeof(buf), "Fonts:  > %s all", allFonts ? "Deselect" : "Select");
        M5.Display.drawString(buf, leftX + indent, y);
        addUiItem(y, UI_LINE_H, ID_FONT_SELALL);
        y += UI_LINE_H;

        // Font checkboxes
        for (int i = startFont; i < endFont; i++) {
            snprintf(buf, sizeof(buf), "%s %s",
                     config.fontEnabled[i] ? "(*)" : "( )",
                     fontNames[i].c_str());
            String s = shortenText(String(buf), maxTextW);
            M5.Display.drawString(s.c_str(), leftX, y);
            addUiItem(y, UI_LINE_H, ID_FONT_BASE + i);
            y += UI_LINE_H;
        }

        // Font page navigation (only if paginated)
        if (totalPages > 1) {
            if (fontPage > 0) {
                M5.Display.drawString("<<<", leftX, y);
            }
            M5.Display.setTextDatum(top_center);
            snprintf(buf, sizeof(buf), "Page %d/%d", fontPage + 1, totalPages);
            M5.Display.drawString(buf, displayW / 2, y);
            if (fontPage < totalPages - 1) {
                M5.Display.setTextDatum(top_right);
                M5.Display.drawString(">>>", displayW - leftX, y);
            }
            addUiItemX(0, y, displayW / 2, UI_LINE_H, ID_FONT_PREV);
            addUiItemX(displayW / 2, y, displayW / 2, UI_LINE_H, ID_FONT_NEXT);
            M5.Display.setTextDatum(top_left);
            y += UI_LINE_H;
        }
    } else {
        // > Fonts link (opens sub-screen)
        int arrowW = uiTextWidth("> ");
        M5.Display.drawString(">", leftX + indent - arrowW, y);
        M5.Display.drawString("Fonts", leftX + indent, y);
        addUiItem(y, UI_LINE_H, ID_FONTS_LINK);
        y += UI_LINE_H;
    }

    // Unicode ranges + Confirm: two lines after fonts, same gap as between groups
    y += UI_PAD;
    int arrowW = uiTextWidth("> ");
    M5.Display.drawString(">", leftX + indent - arrowW, y);
    M5.Display.drawString("Unicode ranges", leftX + indent, y);
    addUiItem(y, UI_LINE_H, ID_UNICODE_LINK);
    y += UI_LINE_H;

    M5.Display.drawString(">", leftX + indent - arrowW, y);
    M5.Display.drawString("Flip interface", leftX + indent, y);
    addUiItem(y, UI_LINE_H, ID_FLIP);
    y += UI_LINE_H;

    M5.Display.drawString(">", leftX + indent - arrowW, y);
    M5.Display.drawString("WiFi manager", leftX + indent, y);
    addUiItem(y, UI_LINE_H, ID_WIFI_MANAGER);
    y += UI_LINE_H;

    y += UI_PAD;  // gap before confirm

    M5.Display.drawString(">", leftX + indent - arrowW, y);
    M5.Display.drawString("Confirm", leftX + indent, y);
    addUiItem(y, UI_LINE_H, ID_CONFIRM);

    uiFlush();
}

// ---- Setup: Fonts sub-screen (when fontCount > 20) ----

#define ID_FSUB_BACK     600
#define ID_FSUB_SELALL   601
#define ID_FSUB_PAGE     602   // left half prev, right half (603) next
#define ID_FSUB_BASE     700   // 700 + fontIndex
#define FONTS_SUB_PER_PAGE 14

void renderFontsScreen(int page) {
    uiItemCount = 0;
    char buf[80];

    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::Font0);

    int maxW = 0;
    int w;

    w = uiTextWidth("> Back"); if (w > maxW) maxW = w;
    w = uiTextWidth("> Deselect all"); if (w > maxW) maxW = w;

    int totalPages = (fontCount + FONTS_SUB_PER_PAGE - 1) / FONTS_SUB_PER_PAGE;
    if (totalPages < 1) totalPages = 1;
    if (page >= totalPages) page = totalPages - 1;
    int startF = page * FONTS_SUB_PER_PAGE;
    int endF = startF + FONTS_SUB_PER_PAGE;
    if (endF > fontCount) endF = fontCount;

    int maxTextW = displayW - 2 * UI_LEFT;
    for (int i = startF; i < endF; i++) {
        snprintf(buf, sizeof(buf), "(*) %s", fontNames[i].c_str());
        String s = shortenText(String(buf), maxTextW);
        w = uiTextWidth(s.c_str()); if (w > maxW) maxW = w;
    }

    int leftX = uiGroupX(maxW);
    if (leftX < UI_LEFT) leftX = UI_LEFT;

    int fontsOnPage = endF - startF;
    int textH = 16;
    int navLineH = (totalPages > 1) ? (UI_LINE_H + UI_PAD) : 0;
    int backLineH = UI_PAD + textH; // last line only needs textH
    int totalContentH = navLineH + (fontsOnPage > 0 ? (fontsOnPage - 1) * UI_LINE_H + textH : 0) + backLineH;

    int titleY = UI_PAD;
    int versionY = displayH - UI_PAD;
    int areaTop = titleY + textH;
    int areaBot = versionY - textH;
    int areaH = areaBot - areaTop;
    int gap = (areaH - totalContentH) / 2;
    if (gap < UI_PAD) gap = UI_PAD;
    int startY = areaTop + gap;

    uiBeginScreen();
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("Fonts", displayW / 2, UI_PAD);
    M5.Display.setTextDatum(bottom_center);
    if (debugMode) {
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "%s - %.0f%%", VERSION, batteryPct);
        M5.Display.drawString(vbuf, displayW / 2, displayH - UI_PAD);
    } else {
        M5.Display.drawString(VERSION, displayW / 2, displayH - UI_PAD);
    }
    M5.Display.setTextDatum(top_left);

    int y = startY;

    // Page navigation (<<< Page N/M >>>) — split left/right for multi-page
    if (totalPages > 1) {
        if (page > 0) {
            M5.Display.drawString("<<<", leftX, y);
        }
        M5.Display.setTextDatum(top_center);
        snprintf(buf, sizeof(buf), "Page %d/%d", page + 1, totalPages);
        M5.Display.drawString(buf, displayW / 2, y);
        if (page < totalPages - 1) {
            M5.Display.setTextDatum(top_right);
            M5.Display.drawString(">>>", displayW - leftX, y);
        }
        addUiItemX(0, y, displayW / 2, UI_LINE_H, ID_FSUB_PAGE);       // left half = prev
        addUiItemX(displayW / 2, y, displayW / 2, UI_LINE_H, ID_FSUB_PAGE + 1); // right half = next
        M5.Display.setTextDatum(top_left);
        y += UI_LINE_H + UI_PAD;
    }

    // Font checkboxes
    for (int i = startF; i < endF; i++) {
        snprintf(buf, sizeof(buf), "%s %s",
                 config.fontEnabled[i] ? "(*)" : "( )",
                 fontNames[i].c_str());
        String s = shortenText(String(buf), maxTextW);
        M5.Display.drawString(s.c_str(), leftX, y);
        addUiItem(y, UI_LINE_H, ID_FSUB_BASE + i);
        y += UI_LINE_H;
    }

    // Back (left) + Select/Deselect (right)
    y += UI_PAD;
    M5.Display.drawString("> Back", leftX, y);
    addUiItemX(0, y, displayW / 2, UI_LINE_H, ID_FSUB_BACK);

    bool allFonts = true;
    for (int i = 0; i < fontCount; i++)
        if (!config.fontEnabled[i]) { allFonts = false; break; }
    snprintf(buf, sizeof(buf), "> %s all", allFonts ? "Deselect" : "Select");
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(buf, displayW - leftX, y);
    addUiItemX(displayW / 2, y, displayW / 2, UI_LINE_H, ID_FSUB_SELALL);
    M5.Display.setTextDatum(top_left);

    uiFlush();
}

// ---- Setup: Unicode ranges screen (2 pages of 14) ----

#define ID_RANGE_BACK    300
#define ID_RANGE_SELALL  301
#define ID_RANGE_BASE    400   // 400 + rangeIndex
#define ID_RANGE_PREV    500
#define ID_RANGE_NEXT    501
#define RANGES_PER_PAGE  14

void renderRangesScreen(int rangePage) {
    uiItemCount = 0;
    char buf[60];

    // -- Pass 1: measure to find widest item --
    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::Font0);

    int maxW = 0;
    int w;

    // Back + Select/Deselect on same line — measure individually
    w = uiTextWidth("> Back"); if (w > maxW) maxW = w;
    w = uiTextWidth("> Deselect all"); if (w > maxW) maxW = w;

    int totalPages = (NUM_GLYPH_RANGES + RANGES_PER_PAGE - 1) / RANGES_PER_PAGE;
    int startR = rangePage * RANGES_PER_PAGE;
    int endR = startR + RANGES_PER_PAGE;
    if (endR > NUM_GLYPH_RANGES) endR = NUM_GLYPH_RANGES;

    for (int ri = startR; ri < endR; ri++) {
        snprintf(buf, sizeof(buf), "(*) %s", glyphRanges[ri].name);
        w = uiTextWidth(buf); if (w > maxW) maxW = w;
    }

    int leftX = uiGroupX(maxW);
    if (leftX < UI_LEFT) leftX = UI_LEFT;

    // -- Calculate total content height for vertical centering --
    // Order: [page nav] + ranges + [back/select]
    int rangesOnPage = endR - startR;
    int textH = 16;
    int navLineH = (totalPages > 1) ? (UI_LINE_H + UI_PAD) : 0; // nav line + gap after
    int backLineH = UI_PAD + textH; // gap before + last line only needs textH
    int totalContentH = navLineH + (rangesOnPage > 0 ? (rangesOnPage - 1) * UI_LINE_H + textH : 0) + backLineH;

    int titleY = UI_PAD;
    int versionY = displayH - UI_PAD;
    int areaTop = titleY + textH;
    int areaBot = versionY - textH;
    int areaH = areaBot - areaTop;
    int gap = (areaH - totalContentH) / 2;
    if (gap < UI_PAD) gap = UI_PAD;
    int startY = areaTop + gap;

    // -- Pass 2: draw --
    uiBeginScreen();
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("Unicode Ranges", displayW / 2, UI_PAD);
    M5.Display.setTextDatum(bottom_center);
    if (debugMode) {
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "%s - %.0f%%", VERSION, batteryPct);
        M5.Display.drawString(vbuf, displayW / 2, displayH - UI_PAD);
    } else {
        M5.Display.drawString(VERSION, displayW / 2, displayH - UI_PAD);
    }
    M5.Display.setTextDatum(top_left);

    int y = startY;

    // Page indicator (top, if multiple pages) — tap anywhere to toggle page
    if (totalPages > 1) {
        M5.Display.setTextDatum(top_center);
        snprintf(buf, sizeof(buf), "Page %d/%d", rangePage + 1, totalPages);
        M5.Display.drawString(buf, displayW / 2, y);
        M5.Display.setTextDatum(top_left);
        addUiItem(y, UI_LINE_H, ID_RANGE_NEXT);
        y += UI_LINE_H + UI_PAD;
    }

    // Range checkboxes
    for (int ri = startR; ri < endR; ri++) {
        snprintf(buf, sizeof(buf), "%s %s",
                 config.rangeEnabled[ri] ? "(*)" : "( )",
                 glyphRanges[ri].name);
        M5.Display.drawString(buf, leftX, y);
        addUiItem(y, UI_LINE_H, ID_RANGE_BASE + ri);
        y += UI_LINE_H;
    }

    // Back (left half) + Select/Deselect (right half) at bottom
    y += UI_PAD;
    M5.Display.drawString("> Back", leftX, y);
    addUiItemX(0, y, displayW / 2, UI_LINE_H, ID_RANGE_BACK);

    bool allRanges = true;
    for (int i = 0; i < NUM_GLYPH_RANGES; i++)
        if (!config.rangeEnabled[i]) { allRanges = false; break; }
    snprintf(buf, sizeof(buf), "> %s all", allRanges ? "Deselect" : "Select");
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(buf, displayW - leftX, y);
    addUiItemX(displayW / 2, y, displayW / 2, UI_LINE_H, ID_RANGE_SELALL);
    M5.Display.setTextDatum(top_left);

    uiFlush();
}

// ---- Setup loop ----

void runSetupScreen() {
    int fontPage = 0;
    unsigned long lastActivity = millis();
    const unsigned long AUTO_CONFIRM_MS = 60000;

    setupFirstRender = true; // First render of setup = full quality
    renderSetupScreen(fontPage);

    while (true) {
        M5.update();

        if (millis() - lastActivity >= AUTO_CONFIRM_MS) {
            Serial.println("Setup: auto-confirm timeout");
            break;
        }

        // Physical button = confirm
        if (M5.BtnA.wasPressed()) {
            Serial.println("Setup: button confirm");
            break;
        }

        auto touch = M5.Touch.getDetail();
        if (!touch.wasPressed()) { delay(20); continue; }

        lastActivity = millis();
        int tx = touch.x, ty = touch.y;

        int id = hitTestUi(tx, ty);
        bool redraw = true;

        if (id == ID_CONFIRM) {
            Serial.println("Setup: confirmed");
            break;
        }

        if (id == ID_TIMER_15) config.wakeIntervalMinutes = 15;
        else if (id == ID_TIMER_10) config.wakeIntervalMinutes = 10;
        else if (id == ID_TIMER_5)  config.wakeIntervalMinutes = 5;
        else if (id == ID_TIMER_2)  config.wakeIntervalMinutes = 2;
        else if (id == ID_TIMER_1)  config.wakeIntervalMinutes = 1;
        else if (id == ID_DIFF_FONT) config.allowDifferentFont = !config.allowDifferentFont;
        else if (id == ID_DIFF_MODE) config.allowDifferentMode = !config.allowDifferentMode;
        else if (id == ID_FONT_SELALL) {
            bool all = true;
            for (int i = 0; i < fontCount; i++)
                if (!config.fontEnabled[i]) { all = false; break; }
            for (int i = 0; i < fontCount; i++) config.fontEnabled[i] = !all;
        }
        else if (id >= ID_FONT_BASE && id < ID_FONT_PREV) {
            int fi = id - ID_FONT_BASE;
            config.fontEnabled[fi] = !config.fontEnabled[fi];
            // Ensure at least one font enabled
            bool any = false;
            for (int i = 0; i < fontCount; i++)
                if (config.fontEnabled[i]) { any = true; break; }
            if (!any) config.fontEnabled[fi] = true;
        }
        else if (id == ID_FONT_PREV || id == ID_FONT_NEXT) {
            // Left third = prev, right third = next
            if (tx < displayW / 3 && fontPage > 0) fontPage--;
            else if (tx > displayW * 2 / 3) fontPage++; // renderSetupScreen clamps
        }
        else if (id == ID_FLIP) {
            config.flipInterface = !config.flipInterface;
            applyRotation();
        }
        else if (id == ID_WIFI_MANAGER) {
            // Save current config first, then launch WiFi manager
            saveConfig();
            runWiFiFontManager();
            // runWiFiFontManager calls ESP.restart(), so we never return here
        }
        else if (id == ID_FONTS_LINK) {
            // Enter fonts sub-screen (>20 fonts)
            int fSubPage = 0;
            renderFontsScreen(fSubPage);
            bool inFonts = true;
            while (inFonts) {
                M5.update();
                if (millis() - lastActivity >= AUTO_CONFIRM_MS) { inFonts = false; break; }
                if (M5.BtnA.wasPressed()) { inFonts = false; break; }

                auto ft = M5.Touch.getDetail();
                if (!ft.wasPressed()) { delay(20); continue; }

                lastActivity = millis();
                int ftx = ft.x, fty = ft.y;
                int fid = hitTestUi(ftx, fty);
                bool fRedraw = true;

                if (fid == ID_FSUB_BACK) {
                    inFonts = false;
                }
                else if (fid == ID_FSUB_SELALL) {
                    bool all = true;
                    for (int i = 0; i < fontCount; i++)
                        if (!config.fontEnabled[i]) { all = false; break; }
                    for (int i = 0; i < fontCount; i++) config.fontEnabled[i] = !all;
                }
                else if (fid == ID_FSUB_PAGE) {
                    // Left half = prev page
                    if (fSubPage > 0) fSubPage--;
                    else fRedraw = false;
                }
                else if (fid == ID_FSUB_PAGE + 1) {
                    // Right half = next page
                    int tp = (fontCount + FONTS_SUB_PER_PAGE - 1) / FONTS_SUB_PER_PAGE;
                    if (fSubPage < tp - 1) fSubPage++;
                    else fRedraw = false;
                }
                else if (fid >= ID_FSUB_BASE && fid < ID_FSUB_BASE + fontCount) {
                    int fi = fid - ID_FSUB_BASE;
                    config.fontEnabled[fi] = !config.fontEnabled[fi];
                    // Ensure at least one font enabled
                    bool any = false;
                    for (int i = 0; i < fontCount; i++)
                        if (config.fontEnabled[i]) { any = true; break; }
                    if (!any) config.fontEnabled[fi] = true;
                }
                else fRedraw = false;

                if (fRedraw) {
                    M5.Display.waitDisplay();
                    renderFontsScreen(fSubPage);
                }
            }
            // Back to main setup screen
            M5.Display.waitDisplay();
        }
        else if (id == ID_UNICODE_LINK) {
            // Enter ranges sub-screen
            int rangePage = 0;
            renderRangesScreen(rangePage);
            bool inRanges = true;
            while (inRanges) {
                M5.update();
                if (millis() - lastActivity >= AUTO_CONFIRM_MS) { inRanges = false; break; }
                if (M5.BtnA.wasPressed()) { inRanges = false; break; }

                auto rt = M5.Touch.getDetail();
                if (!rt.wasPressed()) { delay(20); continue; }
                lastActivity = millis();

                int rtx = rt.x, rty = rt.y;
                int rid = hitTestUi(rtx, rty);
                bool rRedraw = true;

                if (rid == ID_RANGE_BACK) {
                    inRanges = false; rRedraw = false;
                }
                else if (rid == ID_RANGE_SELALL) {
                    bool all = true;
                    for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                        if (!config.rangeEnabled[i]) { all = false; break; }
                    for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                        config.rangeEnabled[i] = !all;
                }
                else if (rid >= ID_RANGE_BASE && rid < ID_RANGE_PREV) {
                    int ri = rid - ID_RANGE_BASE;
                    config.rangeEnabled[ri] = !config.rangeEnabled[ri];
                    bool any = false;
                    for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                        if (config.rangeEnabled[i]) { any = true; break; }
                    if (!any) config.rangeEnabled[ri] = true;
                }
                else if (rid == ID_RANGE_NEXT) {
                    // Toggle between page 0 and page 1
                    rangePage = (rangePage == 0) ? 1 : 0;
                }
                else rRedraw = false;

                if (rRedraw) {
                    M5.Display.waitDisplay();
                    renderRangesScreen(rangePage);
                }
            }
        }
        else redraw = false;

        if (redraw) {
            M5.Display.waitDisplay();
            renderSetupScreen(fontPage);
        }

        delay(20);
    }

    saveConfig();
}

// ---- Sleep ----

// NVS-based wake detection using expected wake timestamp
// Before sleep: save RTC time + timer duration as "expected wake time"
// On boot: compare current RTC time with expected wake time
//   - Close match (within 4s) = timer wake
//   - Far off (button pressed early) = manual wake

// Convert RTC date+time to a flat minute count for easy comparison
// (only needs to be consistent within a day, not absolute)
uint32_t rtcToSeconds() {
    auto dt = M5.Rtc.getDateTime();
    return (uint32_t)dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
}

// saveExpectedWakeTime is now inline in doSleepAt()

// Returns: 0 = not sleeping (cold boot), 1 = timer wake, 2 = manual wake (button)
// Also stores the expected wake time in lastExpectedWakeSec for anchored sleep calc
// Reads last_font and last_mode in the same NVS open to avoid reopening later
static uint32_t lastExpectedWakeSec = 0;
static uint32_t earlyBootRtcSec = 0; // captured immediately after M5.begin, before slow ops
static int nvsLastFont = 0;
static int nvsLastMode = VIEW_BITMAP;

int checkWakeReason() {
    Preferences prefs;
    prefs.begin("ps3", false);
    bool wasSleeping = prefs.getBool("sleeping", false);
    if (!wasSleeping) {
        prefs.end();
        return 0; // never went to sleep (first boot, or NVS cleared)
    }

    lastExpectedWakeSec = prefs.getUInt("wake_sec", 0);
    nvsLastFont = prefs.getInt("last_font", 0);
    nvsLastMode = prefs.getInt("last_mode", VIEW_BITMAP);
    // Clear flag
    prefs.putBool("sleeping", false);
    prefs.end();

    // Use earlyBootRtcSec captured right after M5.begin, not the current time
    // (scanFonts with 100 fonts can take 5-10s and would skew the diff)
    uint32_t nowSec = earlyBootRtcSec;

    // Calculate difference handling midnight wrap
    int32_t diff = (int32_t)nowSec - (int32_t)lastExpectedWakeSec;
    if (diff > 43200) diff -= 86400;
    if (diff < -43200) diff += 86400;

    // RTC alarm fires at exact HH:MM:00, boot takes ~2-3s, so timer wake
    // diff is always positive (+2-4s). A negative diff (woke before expected)
    // is always a manual button press — the RTC can't fire early.
    #define WAKE_TOLERANCE_S 4
    int result;
    if (diff >= 0 && diff <= WAKE_TOLERANCE_S) {
        result = 1; // timer wake
    } else {
        result = 2; // manual wake (button pressed or woke before expected time)
    }

    Serial.printf("Wake check: now=%02d:%02d:%02d, expected=%02d:%02d:%02d, diff=%ds → %s\n",
                  nowSec/3600, (nowSec%3600)/60, nowSec%60,
                  lastExpectedWakeSec/3600, (lastExpectedWakeSec%3600)/60, lastExpectedWakeSec%60,
                  diff, result == 1 ? "TIMER" : "MANUAL");

    return result;
}

// Core sleep function — sets RTC alarm for exact wake at wakeTargetSec (seconds since midnight).
// Waits for RTC :00 boundary, then sets alarm on hour:minute match.
// Debug battery log — appends a line to /battery_log.txt on each glyph generation
// Format: HH:MM:SS | U+XXXX | FontName | mode | battery%
// Empty line before manual wake entries to separate intervals
void logBatteryDebug(uint32_t codepoint, const char* wakeType) {
    if (!debugMode) return;

    // If fonts are in flash, we need to init SD just for the log
    // Skip if SD is not physically present (avoid SPI conflicts)
    bool sdWasOff = false;
    if (fontsInFlash) {
        SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
        if (SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
            sdWasOff = true;
        } else {
            SD.end();
            SPI.end();
            Serial.println("SD not available for battery log — skipping");
            return;
        }
    }

    m5::rtc_time_t t;
    M5.Rtc.getTime(&t);

    File logf = SD.open("/battery_log.txt", FILE_APPEND);
    if (!logf) {
        // File doesn't exist yet — create it
        logf = SD.open("/battery_log.txt", FILE_WRITE);
    }
    if (logf) {
        logf.printf("%02d:%02d:%02d | U+%04X | %s | %s | %.1f%%  [%s]\n",
                    t.hours, t.minutes, t.seconds,
                    codepoint,
                    fontNames[currentFontIndex].c_str(),
                    currentViewMode == VIEW_BITMAP ? "bitmap" : "outline",
                    batteryPct,
                    wakeType);
        logf.close();
        Serial.println("Battery log written to /battery_log.txt");
    }

    // Close SD again if we opened it just for the log
    if (sdWasOff) {
        SD.end();
        SPI.end();
    }
}

// Append an empty line to the battery log (separates manual wake sessions)
void logBatteryBlankLine() {
    if (!debugMode) return;

    bool sdWasOff = false;
    if (fontsInFlash) {
        SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
        if (SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
            sdWasOff = true;
        } else {
            SD.end();
            SPI.end();
            return;
        }
    }

    File logf = SD.open("/battery_log.txt", FILE_APPEND);
    if (logf) {
        logf.println();
        logf.close();
    }

    if (sdWasOff) { SD.end(); SPI.end(); }
}

void doSleepAt(uint32_t wakeTargetSec) {
    int wakeH = wakeTargetSec / 3600;
    int wakeM = (wakeTargetSec % 3600) / 60;

    Serial.printf("Sleep target: wake at %02d:%02d:00\n", wakeH, wakeM);

    // Wait for display refresh to finish
    M5.Display.waitDisplay();

    // Clean up FreeType and font file
    if (ftFace) { FT_Done_Face(ftFace); ftFace = nullptr; }
    if (fontFile) { fontFile.close(); }
    if (ftLibrary) { FT_Done_FreeType(ftLibrary); ftLibrary = nullptr; }

    // Cleanly close storage
    if (fontsInFlash && flashInitialized) {
        LittleFS.end();
    } else {
        SD.end();
    }

    uint32_t nowSec = rtcToSeconds();
    Serial.printf("Now %02d:%02d:%02d, wake alarm set for %02d:%02d:00\n",
                  nowSec/3600, (nowSec%3600)/60, nowSec%60, wakeH, wakeM);
    Serial.flush();

    // Save expected wake time, interval, and current state to NVS
    {
        uint32_t wakeExact = (uint32_t)wakeH * 3600 + wakeM * 60;
        Preferences prefs;
        prefs.begin("ps3", false);
        prefs.putBool("sleeping", true);
        prefs.putUInt("wake_sec", wakeExact);
        prefs.putUInt("wake_int", config.wakeIntervalMinutes);
        // Save current font/mode/appMode so timer wake can preserve them if allow* is off
        prefs.putInt("last_font", currentFontIndex);
        prefs.putInt("last_mode", (int)currentViewMode);
        prefs.end();
        Serial.printf("Expected wake at %02d:%02d:00 (interval %dmin)\n",
                      wakeH, wakeM, config.wakeIntervalMinutes);
    }

    // Use RTC alarm (hour:minute match) — fires exactly when HH:MM matches
    m5::rtc_time_t wakeTime;
    wakeTime.hours = wakeH;
    wakeTime.minutes = wakeM;
    wakeTime.seconds = 0;

    M5.Power.timerSleep(wakeTime);
    esp_deep_sleep_start();
}

// Fresh sleep: wake at now + interval (rounded up to next minute boundary)
// Used after: manual wake inactivity, first boot, setup restart
void goToSleep() {
    int intervalMin = config.wakeIntervalMinutes;
    uint32_t nowSec = rtcToSeconds();
    // Round up to next minute boundary, then add interval
    uint32_t nowMin = (nowSec + 59) / 60; // ceiling to next minute
    uint32_t wakeTargetSec = ((nowMin + intervalMin) * 60) % 86400;

    Serial.printf("Fresh sleep — %d minutes from now\n", intervalMin);
    doSleepAt(wakeTargetSec);
}

// Anchored sleep: next wake = lastExpectedWake + interval
// Keeps the schedule aligned so there's no drift across multiple cycles
// Used after: timer wake (glyph rendered, go back to sleep on schedule)
void goToSleepAnchored() {
    int intervalSec = config.wakeIntervalMinutes * 60;
    uint32_t nextWakeSec = (lastExpectedWakeSec + intervalSec) % 86400;
    uint32_t nowSec = rtcToSeconds();

    // If target is in the past or in the SAME minute we're in now, skip to next interval
    // (RTC alarm matches HH:MM — won't fire if we're already in that minute)
    int32_t untilWake = (int32_t)nextWakeSec - (int32_t)nowSec;
    if (untilWake < 0) untilWake += 86400;
    uint32_t nowMin = nowSec / 60;
    uint32_t targetMin = nextWakeSec / 60;
    if (nowMin == targetMin || untilWake <= 0) {
        Serial.printf("Anchored: target %02d:%02d same minute as now, skipping to next\n",
                      nextWakeSec/3600, (nextWakeSec%3600)/60);
        nextWakeSec = (nextWakeSec + intervalSec) % 86400;
        untilWake += intervalSec;
    }
    if (untilWake > intervalSec * 2) {
        Serial.println("Anchored sleep: time looks wrong, using fresh sleep");
        goToSleep();
        return;
    }

    Serial.printf("Anchored sleep — next wake at %02d:%02d:00 (in ~%ds)\n",
                  nextWakeSec/3600, (nextWakeSec%3600)/60, untilWake);
    doSleepAt(nextWakeSec);
}

// Ensure at least one font is enabled; if none, enable all
void ensureFontsEnabled() {
    for (int i = 0; i < fontCount; i++) {
        if (config.fontEnabled[i]) return; // at least one is enabled
    }
    // None enabled — enable all
    for (int i = 0; i < fontCount; i++) config.fontEnabled[i] = true;
}

// Pick a random enabled font index
int pickRandomEnabledFont() {
    ensureFontsEnabled();
    int enabled[MAX_FONTS];
    int count = 0;
    for (int i = 0; i < fontCount; i++) {
        if (config.fontEnabled[i]) enabled[count++] = i;
    }
    return enabled[random(count)];
}

// ---- Splash + Setup sequence (used at first boot and on long hold) ----

// QR Code bitmap for GitHub repository
// URL: https://github.com/marcelloemme/PaperSpecimenS3
// Size: 29x29 modules (Version 3 QR Code)
const uint8_t QR_SIZE = 29;
const uint8_t qrcode_data[] PROGMEM = {
    0b11111110, 0b11110100, 0b01100011, 0b11111000,  // Row 0
    0b10000010, 0b00011010, 0b11111010, 0b00001000,  // Row 1
    0b10111010, 0b01010101, 0b00000010, 0b11101000,  // Row 2
    0b10111010, 0b00110000, 0b11000010, 0b11101000,  // Row 3
    0b10111010, 0b00111000, 0b11110010, 0b11101000,  // Row 4
    0b10000010, 0b01010100, 0b11001010, 0b00001000,  // Row 5
    0b11111110, 0b10101010, 0b10101011, 0b11111000,  // Row 6
    0b00000000, 0b11001100, 0b00011000, 0b00000000,  // Row 7
    0b11011010, 0b01001011, 0b01010010, 0b00001000,  // Row 8
    0b10000001, 0b10100001, 0b01110101, 0b10110000,  // Row 9
    0b01100011, 0b01001011, 0b11000001, 0b10100000,  // Row 10
    0b11111000, 0b00000000, 0b11101111, 0b01001000,  // Row 11
    0b10101011, 0b11101001, 0b10001011, 0b00001000,  // Row 12
    0b11101001, 0b00101111, 0b11100011, 0b11111000,  // Row 13
    0b10010011, 0b01100111, 0b10011110, 0b10101000,  // Row 14
    0b00101001, 0b00011000, 0b10110001, 0b10101000,  // Row 15
    0b10111010, 0b11010101, 0b10011100, 0b01000000,  // Row 16
    0b11001101, 0b01101000, 0b00110000, 0b10110000,  // Row 17
    0b11010011, 0b00110011, 0b00010000, 0b11001000,  // Row 18
    0b11101000, 0b00111101, 0b10010010, 0b01100000,  // Row 19
    0b11110010, 0b01100010, 0b11001111, 0b11110000,  // Row 20
    0b00000000, 0b11100111, 0b10101000, 0b11000000,  // Row 21
    0b11111110, 0b00011011, 0b11011010, 0b11000000,  // Row 22
    0b10000010, 0b00000010, 0b01011000, 0b10010000,  // Row 23
    0b10111010, 0b11101001, 0b10011111, 0b11000000,  // Row 24
    0b10111010, 0b10010011, 0b11100100, 0b00001000,  // Row 25
    0b10111010, 0b01100111, 0b00001101, 0b10111000,  // Row 26
    0b10000010, 0b11100000, 0b00000011, 0b01101000,  // Row 27
    0b11111110, 0b11111100, 0b00011010, 0b10000000   // Row 28
};

// Draw QR code centered at (centerX, centerY) with given pixel size per module
void drawQRCode(int centerX, int centerY, int pixelSize) {
    int qrDisplaySize = QR_SIZE * pixelSize;
    int startX = centerX - qrDisplaySize / 2;
    int startY = centerY - qrDisplaySize / 2;

    for (int moduleY = 0; moduleY < QR_SIZE; moduleY++) {
        for (int moduleX = 0; moduleX < QR_SIZE; moduleX++) {
            int byteIndex = moduleY * 4 + (moduleX / 8);
            int bitIndex = 7 - (moduleX % 8);
            uint8_t b = pgm_read_byte(&qrcode_data[byteIndex]);
            bool isBlack = (b >> bitIndex) & 1;

            if (isBlack) {
                M5.Display.fillRect(
                    startX + moduleX * pixelSize,
                    startY + moduleY * pixelSize,
                    pixelSize, pixelSize, TFT_BLACK);
            }
        }
    }
}

void runSplashAndSetup() {
    // Always try to init SD at splash time, even if fonts were previously in flash.
    // User may have inserted/updated the SD card since last boot.
    bool sdAvailableAtSplash = false;
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        Serial.println("SD not available at splash");
    } else {
        sdAvailableAtSplash = true;
        Serial.println("SD available at splash");
    }

    // Splash screen (5 seconds, 2+ taps = debug mode)
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextSize(2);

    // Title and version at same position as setup screen
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("PaperSpecimen S3", displayW / 2, UI_PAD);
    M5.Display.setTextDatum(bottom_center);
    M5.Display.drawString(VERSION, displayW / 2, displayH - UI_PAD);

    // QR code centered on screen (29 modules × 6px = 174px)
    drawQRCode(displayW / 2, displayH / 2, 6);

    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.display();
    M5.Display.waitDisplay();

    // --- Splash: 5 seconds for debug mode tap detection ---
    int tapCount = 0;
    unsigned long splashStart = millis();
    while (millis() - splashStart < 5000) {
        M5.update();
        auto t = M5.Touch.getDetail();
        if (t.wasPressed()) {
            tapCount++;
            Serial.printf("Splash tap #%d\n", tapCount);
        }
        delay(20);
    }
    if (tapCount >= 4) {
        config.isDebugMode = true;
        debugMode = true;
        Serial.println("*** DEBUG MODE ACTIVATED ***");
        if (config.wakeIntervalMinutes > 5) config.wakeIntervalMinutes = 5;
    } else {
        config.isDebugMode = false;
        debugMode = false;
        // Disable serial now that we know debug is off
        Serial.end();
        // Sanitize: if timer was set to debug-only value, reset to default
        if (config.wakeIntervalMinutes < 5) config.wakeIntervalMinutes = 15;
    }

    // --- After splash: copy fonts from SD to flash if they fit ---
    bool sdHasFonts = false;
    if (sdAvailableAtSplash) {
        File testDir = SD.open("/fonts");
        if (testDir && testDir.isDirectory()) {
            sdHasFonts = true;
            testDir.close();
        }
    }

    if (sdHasFonts) {
        size_t totalFontSize = calcSDFontsSize();
        Serial.printf("SD fonts total size: %d bytes (%.1f MB), flash limit: %.1f MB\n",
                      totalFontSize, totalFontSize / 1048576.0, FLASH_FONT_MAX_BYTES / 1048576.0);

        if (totalFontSize > 0 && totalFontSize <= FLASH_FONT_MAX_BYTES) {
            Serial.println("Fonts fit in flash — copying...");
            if (copyFontsToFlash()) {
                // Also copy config if it exists on SD
                if (SD.exists(CONFIG_FILE)) {
                    copyConfigToFlash();
                }
                fontsInFlash = true;
                Serial.println("Flash copy complete — SD can be powered off");
            }
        } else if (totalFontSize > FLASH_FONT_MAX_BYTES) {
            Serial.printf("Fonts too large for flash (%d > %d) — using SD\n",
                          totalFontSize, (int)FLASH_FONT_MAX_BYTES);
            fontsInFlash = false;

            // Clean up flash to free space (old fonts no longer needed)
            if (!flashInitialized && LittleFS.begin(true)) {
                flashInitialized = true;
            }
            if (flashInitialized) {
                File fdir = LittleFS.open("/fonts");
                if (fdir && fdir.isDirectory()) {
                    File fe;
                    while ((fe = fdir.openNextFile())) {
                        String delPath = String("/fonts/") + fe.name();
                        fe.close();
                        LittleFS.remove(delPath);
                    }
                    fdir.close();
                }
                LittleFS.remove(CONFIG_FILE);
                Serial.println("Flash storage cleared (fonts moved to SD)");
                LittleFS.end();
                flashInitialized = false;
            }
        }
    } else {
        // No SD or no fonts on SD — check if flash has fonts
        if (!flashInitialized && LittleFS.begin(true)) {
            flashInitialized = true;
        }
        if (flashInitialized) {
            // Check if flash has any fonts
            bool flashHasFonts = false;
            File testFlash = LittleFS.open("/fonts");
            if (testFlash && testFlash.isDirectory()) {
                File tf = testFlash.openNextFile();
                if (tf) { flashHasFonts = true; tf.close(); }
                testFlash.close();
            } else if (testFlash) {
                testFlash.close();
            }

            if (flashHasFonts) {
                fontsInFlash = true;
                Serial.println("No SD — using existing flash fonts");
            } else {
                // Flash is empty, extract embedded default fonts
                Serial.println("No SD, no flash fonts — extracting embedded defaults...");
                extractEmbeddedFonts();
                fontsInFlash = true;
                Serial.println("Embedded fonts ready in flash");
            }
        }
    }

    // Save flash state to NVS
    {
        Preferences prefs;
        prefs.begin("ps3", false);
        prefs.putBool("fonts_flash", fontsInFlash);
        prefs.end();
    }

    // If fonts are in flash, we can close SD now
    if (fontsInFlash && sdAvailableAtSplash) {
        SD.end();
        Serial.println("SD card powered off");
    }

    // Scan fonts from the active source (flash or SD)
    scanFonts();

    // Run setup screen
    setupFirstRender = true;
    runSetupScreen();

    // After setup, render first content
    currentFontIndex = 0;
    for (int i = 0; i < fontCount; i++) {
        if (config.fontEnabled[i]) { currentFontIndex = i; break; }
    }

    if (loadFontFromStream(currentFontIndex)) {
        isFirstRender = true;
        uint32_t cp = findRandomGlyph();
        drawGlyph(cp);
        M5.Display.waitDisplay();
    }

    // Flush any queued touch events (e.g. double-tap on Confirm)
    // so they don't trigger unintended actions in loop()
    for (int i = 0; i < 10; i++) { M5.update(); delay(10); }
}

// ---- WiFi Font Manager ----

static const char* WIFI_SSID = "PaperSpecimenS3";
static const char* WIFI_PASS = "seriforsans";
#define WIFI_TIMEOUT_MS 300000 // 5 minutes
#define WIFI_CHANNEL 1
#define DNS_PORT 53

static WebServer wifiServer(80);
static DNSServer dnsServer;
static bool wifiManagerDone = false;
static bool wifiManagesFlash = false; // true when no SD, WiFi manages flash directly

// Helper: get the filesystem the WiFi manager should use
fs::FS& wifiFS() {
    return wifiManagesFlash ? (fs::FS&)LittleFS : (fs::FS&)SD;
}
static unsigned long wifiLastActivity = 0; // reset on each API call to extend timeout

void wifiSendCors() {
    wifiServer.sendHeader("Access-Control-Allow-Origin", "*");
}

void wifiHandleRoot() {
    wifiServer.sendHeader("Content-Encoding", "gzip");
    wifiServer.send_P(200, "text/html", (const char*)web_html_gz, web_html_gz_len);
}

void wifiHandleFonts() {
    wifiSendCors();
    String json = "[";
    File dir = wifiFS().open("/fonts");
    if (dir) {
        bool first = true;
        File entry = dir.openNextFile();
        while (entry) {
            String name = String(entry.name());
            // Skip hidden files (macOS ._files)
            if (!name.startsWith(".") && (name.endsWith(".ttf") || name.endsWith(".TTF") ||
                name.endsWith(".otf") || name.endsWith(".OTF"))) {
                if (!first) json += ",";
                json += "{\"name\":\"" + name + "\",\"size\":" + String(entry.size()) + "}";
                first = false;
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
    }
    json += "]";
    wifiServer.send(200, "application/json", json);
}

void wifiHandleDelete() {
    if (!wifiServer.hasArg("plain")) { wifiServer.send(400, "text/plain", "No body"); return; }
    // Simple JSON parse for {"name":"filename"}
    String body = wifiServer.arg("plain");
    int nameStart = body.indexOf("\"name\"") + 8;
    int nameEnd = body.indexOf("\"", nameStart);
    if (nameStart < 8 || nameEnd < 0) { wifiServer.send(400, "text/plain", "Bad request"); return; }
    String filename = body.substring(nameStart, nameEnd);

    String path = "/fonts/" + filename;
    if (wifiFS().exists(path.c_str())) {
        wifiFS().remove(path.c_str());
        Serial.printf("WiFi: Deleted %s\n", path.c_str());
        wifiServer.send(200, "text/plain", "OK");
    } else {
        wifiServer.send(404, "text/plain", "Not found");
    }
}

void wifiHandleRename() {
    if (!wifiServer.hasArg("plain")) { wifiServer.send(400, "text/plain", "No body"); return; }
    String body = wifiServer.arg("plain");
    // Parse {"name":"old","newName":"new"}
    int nameStart = body.indexOf("\"name\"") + 8;
    int nameEnd = body.indexOf("\"", nameStart);
    int newStart = body.indexOf("\"newName\"") + 11;
    int newEnd = body.indexOf("\"", newStart);
    if (nameStart < 8 || newStart < 11) { wifiServer.send(400, "text/plain", "Bad request"); return; }
    String oldName = body.substring(nameStart, nameEnd);
    String newName = body.substring(newStart, newEnd);

    String oldPath = "/fonts/" + oldName;
    String newPath = "/fonts/" + newName;
    if (wifiFS().exists(oldPath.c_str())) {
        if (wifiFS().rename(oldPath.c_str(), newPath.c_str())) {
            Serial.printf("WiFi: Renamed %s -> %s\n", oldPath.c_str(), newPath.c_str());
            wifiServer.send(200, "text/plain", "OK");
        } else {
            Serial.printf("WiFi: Rename FAILED %s -> %s\n", oldPath.c_str(), newPath.c_str());
            wifiServer.send(500, "text/plain", "Rename failed");
        }
    } else {
        wifiServer.send(404, "text/plain", "Not found");
    }
}

void wifiHandleUpload() {
    wifiLastActivity = millis(); // reset on each chunk
    HTTPUpload& upload = wifiServer.upload();
    static File uploadFile;

    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (filename.startsWith("/")) filename = filename.substring(1);
        // Reject non-font files
        String lower = filename;
        lower.toLowerCase();
        if (!lower.endsWith(".ttf") && !lower.endsWith(".otf")) {
            Serial.printf("WiFi: Rejected non-font file: %s\n", filename.c_str());
            return;
        }
        String path = "/fonts/" + filename;
        Serial.printf("WiFi: Upload start %s\n", path.c_str());
        // Check flash space if managing flash directly
        if (wifiManagesFlash) {
            size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
            Serial.printf("WiFi: Flash free: %d bytes\n", freeBytes);
            // We can't know the total size upfront with chunked uploads,
            // so we'll check during write and abort if full
        }
        uploadFile = wifiFS().open(path.c_str(), FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.printf("WiFi: Upload complete (%d bytes)\n", upload.totalSize);
        }
    }
}

void wifiHandleUploadComplete() {
    wifiServer.send(200, "text/plain", "OK");
}

void wifiHandleApply() {
    wifiServer.send(200, "text/plain", "OK");
    delay(500);
    wifiManagerDone = true; // signal main loop to exit
}

void wifiHandleTimeout() {
    wifiServer.send(200, "text/plain", "OK");
    delay(200);
    wifiManagerDone = true;
}

void wifiHandleInfo() {
    wifiSendCors();
    String json = "{\"flash\":" + String(wifiManagesFlash ? "true" : "false");
    if (wifiManagesFlash) {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        json += ",\"flash_total\":" + String(total);
        json += ",\"flash_used\":" + String(used);
        json += ",\"flash_free\":" + String(total - used);
    }
    json += "}";
    wifiServer.send(200, "application/json", json);
}

// --- OTA Firmware Update ---
// GitHub API URL for latest release
#define OTA_GITHUB_API "https://api.github.com/repos/marcelloemme/PaperSpecimenS3/releases/latest"
static String otaDownloadUrl = ""; // stored between check and update

void wifiHandleWifiScan() {
    wifiSendCors();
    Serial.println("WiFi: Scanning networks...");
    int n = WiFi.scanNetworks(false, false, false, 300);
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "\\\""); // escape quotes in SSID
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i));
        json += ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    Serial.printf("WiFi: Found %d networks\n", n);
    wifiServer.send(200, "application/json", json);
}

void wifiHandleOtaCheck() {
    wifiSendCors();
    if (!wifiServer.hasArg("plain")) { wifiServer.send(400, "application/json", "{\"error\":\"No body\"}"); return; }
    String body = wifiServer.arg("plain");

    // Parse SSID and password from JSON
    int ssidStart = body.indexOf("\"ssid\"") + 8;
    int ssidEnd = body.indexOf("\"", ssidStart);
    int passStart = body.indexOf("\"pass\"") + 8;
    int passEnd = body.indexOf("\"", passStart);
    if (ssidStart < 8 || ssidEnd < 0) { wifiServer.send(400, "application/json", "{\"error\":\"Bad request\"}"); return; }

    String ssid = body.substring(ssidStart, ssidEnd);
    String pass = (passStart >= 8 && passEnd > passStart) ? body.substring(passStart, passEnd) : "";

    Serial.printf("OTA: Connecting to '%s'...\n", ssid.c_str());

    // Update e-ink display
    M5.Display.wakeup();
    int cx = displayW / 2;
    M5.Display.fillRect(0, displayH / 2 - 30, displayW, 60, TFT_WHITE);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("Connecting to WiFi...", cx, displayH / 2 - 8);
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
    M5.Display.display();
    M5.Display.waitDisplay();

    // Switch to AP+STA mode (keep AP running, connect to router as client)
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("OTA: WiFi connection failed");
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        // Restore e-ink
        M5.Display.fillRect(0, displayH / 2 - 30, displayW, 60, TFT_WHITE);
        M5.Display.drawString("Connection failed", cx, displayH / 2 - 8);
        M5.Display.setEpdMode(epd_mode_t::epd_fastest);
        M5.Display.display();
        wifiServer.send(200, "application/json", "{\"error\":\"Could not connect to WiFi. Check password.\"}");
        return;
    }
    Serial.printf("OTA: Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Update e-ink
    M5.Display.fillRect(0, displayH / 2 - 30, displayW, 60, TFT_WHITE);
    M5.Display.drawString("Checking for updates...", cx, displayH / 2 - 8);
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
    M5.Display.display();
    M5.Display.waitDisplay();

    // Query GitHub API for latest release
    HTTPClient http;
    http.setUserAgent("PaperSpecimenS3");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);

    if (!http.begin(OTA_GITHUB_API)) {
        Serial.println("OTA: HTTP begin failed");
        wifiServer.send(200, "application/json", "{\"error\":\"Failed to connect to GitHub\"}");
        return;
    }

    int httpCode = http.GET();
    Serial.printf("OTA: GitHub API response: %d\n", httpCode);

    if (httpCode != 200) {
        http.end();
        wifiServer.send(200, "application/json", "{\"error\":\"GitHub API returned " + String(httpCode) + "\"}");
        return;
    }

    String payload = http.getString();
    http.end();

    // Parse tag_name from JSON (simple string search)
    int tagStart = payload.indexOf("\"tag_name\"") + 13;
    int tagEnd = payload.indexOf("\"", tagStart);
    String latestTag = payload.substring(tagStart, tagEnd);
    Serial.printf("OTA: Latest release: %s, current: %s\n", latestTag.c_str(), VERSION);

    // Find .bin asset download URL
    // Look for browser_download_url ending in .bin
    otaDownloadUrl = "";
    int binUrlStart = payload.indexOf("browser_download_url");
    while (binUrlStart > 0) {
        binUrlStart = payload.indexOf("\"", binUrlStart + 22) + 1;
        int binUrlEnd = payload.indexOf("\"", binUrlStart);
        String url = payload.substring(binUrlStart, binUrlEnd);
        if (url.endsWith(".bin")) {
            otaDownloadUrl = url;
            break;
        }
        binUrlStart = payload.indexOf("browser_download_url", binUrlEnd);
    }

    bool updateAvailable = (latestTag != VERSION && otaDownloadUrl.length() > 0);

    String response = "{\"current\":\"" + String(VERSION) + "\",\"latest\":\"" + latestTag + "\"";
    response += ",\"update_available\":" + String(updateAvailable ? "true" : "false") + "}";

    // Update e-ink
    M5.Display.fillRect(0, displayH / 2 - 30, displayW, 60, TFT_WHITE);
    if (updateAvailable) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Update available: %s", latestTag.c_str());
        M5.Display.drawString(msg, cx, displayH / 2 - 8);
    } else {
        M5.Display.drawString("Up to date", cx, displayH / 2 - 8);
    }
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
    M5.Display.display();

    wifiServer.send(200, "application/json", response);
}

void wifiHandleOtaUpdate() {
    wifiSendCors();

    if (otaDownloadUrl.length() == 0) {
        wifiServer.send(200, "application/json", "{\"error\":\"No update URL. Run check first.\"}");
        return;
    }

    Serial.printf("OTA: Downloading %s\n", otaDownloadUrl.c_str());

    // Update e-ink
    int cx = displayW / 2;
    M5.Display.wakeup();
    M5.Display.fillRect(0, displayH / 2 - 30, displayW, 60, TFT_WHITE);
    M5.Display.drawString("Downloading update...", cx, displayH / 2 - 8);
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
    M5.Display.display();
    M5.Display.waitDisplay();

    HTTPClient http;
    http.setUserAgent("PaperSpecimenS3");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(30000);

    if (!http.begin(otaDownloadUrl)) {
        wifiServer.send(200, "application/json", "{\"error\":\"Failed to connect to download server\"}");
        return;
    }

    int httpCode = http.GET();
    if (httpCode != 200) {
        http.end();
        wifiServer.send(200, "application/json", "{\"error\":\"Download failed: HTTP " + String(httpCode) + "\"}");
        return;
    }

    int contentLength = http.getSize();
    Serial.printf("OTA: Firmware size: %d bytes\n", contentLength);

    if (contentLength <= 0) {
        http.end();
        wifiServer.send(200, "application/json", "{\"error\":\"Invalid firmware size\"}");
        return;
    }

    if (!Update.begin(contentLength)) {
        http.end();
        wifiServer.send(200, "application/json", "{\"error\":\"Not enough space for update\"}");
        return;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[4096];
    int written = 0;
    int lastPct = -1;

    while (written < contentLength) {
        int available = stream->available();
        if (available <= 0) {
            delay(10);
            continue;
        }
        int toRead = min(available, (int)sizeof(buf));
        if (toRead > contentLength - written) toRead = contentLength - written;
        int bytesRead = stream->read(buf, toRead);
        if (bytesRead <= 0) break;

        Update.write(buf, bytesRead);
        written += bytesRead;

        int pct = (written * 100) / contentLength;
        if (pct != lastPct && pct % 10 == 0) {
            lastPct = pct;
            Serial.printf("OTA: %d%%\n", pct);
            // Update e-ink progress
            char prog[32];
            snprintf(prog, sizeof(prog), "Updating... %d%%", pct);
            M5.Display.fillRect(0, displayH / 2 - 30, displayW, 60, TFT_WHITE);
            M5.Display.setTextDatum(top_center);
            M5.Display.drawString(prog, cx, displayH / 2 - 8);
            M5.Display.setEpdMode(epd_mode_t::epd_fastest);
            M5.Display.display();
        }
    }
    http.end();

    if (Update.end(true)) {
        Serial.println("OTA: Update successful!");
        // Show success on e-ink
        M5.Display.fillRect(0, displayH / 2 - 30, displayW, 60, TFT_WHITE);
        M5.Display.drawString("Update complete! Restarting...", cx, displayH / 2 - 8);
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.display();
        M5.Display.waitDisplay();

        wifiServer.send(200, "application/json", "{\"success\":true}");
        delay(1000);
        ESP.restart();
    } else {
        Serial.printf("OTA: Update failed: %s\n", Update.errorString());
        wifiServer.send(200, "application/json", "{\"error\":\"" + String(Update.errorString()) + "\"}");
    }
}

// Captive portal: redirect all unknown requests to root
void wifiHandleNotFound() {
    wifiServer.sendHeader("Location", "http://192.168.4.1/", true);
    wifiServer.send(302, "text/plain", "");
}

void runWiFiFontManager() {
    Serial.println("Starting WiFi Font Manager...");

    // Close any open font file (FT_Open_Face stream) before managing files
    if (ftFace) { FT_Done_Face(ftFace); ftFace = nullptr; }
    if (fontFile) { fontFile.close(); }
    if (ftLibrary) { FT_Done_FreeType(ftLibrary); ftLibrary = nullptr; }

    // Try to init SD for WiFi file operations
    bool sdAvailable = false;
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        sdAvailable = true;
        wifiManagesFlash = false;
        Serial.println("WiFi: SD available — managing SD fonts");
    } else {
        // No SD — manage flash directly
        wifiManagesFlash = true;
        if (!flashInitialized) {
            LittleFS.begin(true);
            flashInitialized = true;
        }
        LittleFS.mkdir("/fonts");
        Serial.println("WiFi: No SD — managing flash fonts directly");
    }

    // Boost CPU for WiFi
    setCpuFrequencyMhz(160);

    // Enable WiFi
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL, 0, 4); // max 4 connections
    delay(500);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("WiFi AP: SSID=%s, IP=%s\n", WIFI_SSID, ip.toString().c_str());

    // Start DNS server for captive portal (redirect all domains to our IP)
    dnsServer.start(DNS_PORT, "*", ip);

    // Register routes
    wifiServer.on("/", HTTP_GET, wifiHandleRoot);
    wifiServer.on("/api/fonts", HTTP_GET, wifiHandleFonts);
    wifiServer.on("/api/delete", HTTP_POST, wifiHandleDelete);
    wifiServer.on("/api/rename", HTTP_POST, wifiHandleRename);
    wifiServer.on("/api/upload", HTTP_POST, wifiHandleUploadComplete, wifiHandleUpload);
    wifiServer.on("/api/apply", HTTP_GET, wifiHandleApply);
    wifiServer.on("/api/info", HTTP_GET, wifiHandleInfo);
    wifiServer.on("/api/timeout", HTTP_GET, wifiHandleTimeout);
    wifiServer.on("/api/wifi-scan", HTTP_GET, wifiHandleWifiScan);
    wifiServer.on("/api/ota-check", HTTP_POST, wifiHandleOtaCheck);
    wifiServer.on("/api/ota-update", HTTP_GET, wifiHandleOtaUpdate);
    wifiServer.onNotFound(wifiHandleNotFound);
    wifiServer.begin();
    Serial.println("Web server started on port 80");

    // Show WiFi info on e-ink display
    M5.Display.wakeup();
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::Font0);

    int cx = displayW / 2;

    // Title at top, version at bottom (fixed)
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("PaperSpecimen S3", cx, UI_PAD);
    M5.Display.setTextDatum(bottom_center);
    M5.Display.drawString(VERSION, cx, displayH - UI_PAD);

    // Center content vertically between title and version
    // Content: timer line + blank + SSID label + SSID + blank + Pass label + Pass + blank + URL label + URL + fallback
    int lineH = 30;
    int gapH = 20;
    int textH = 16; // actual text height at textSize 2, Font0
    // 7 gaps between 8 lines + 3 extra gaps between groups + last line is just textH
    int contentH = lineH * 7 + textH + gapH * 3;
    int titleBottom = UI_PAD + textH;
    int versionTop = displayH - UI_PAD - textH;
    int areaH = versionTop - titleBottom;
    int startY = titleBottom + (areaH - contentH) / 2;

    int y = startY;
    // Timer line Y — we'll update this area every second
    int timerY = y;
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("WiFi Font Manager - 5m 00s", cx, timerY);
    y += lineH + gapH;

    M5.Display.drawString("SSID:", cx, y);
    y += lineH;
    M5.Display.drawString(WIFI_SSID, cx, y);
    y += lineH + gapH;

    M5.Display.drawString("Password:", cx, y);
    y += lineH;
    M5.Display.drawString(WIFI_PASS, cx, y);
    y += lineH + gapH;

    M5.Display.drawString("Open in browser:", cx, y);
    y += lineH;
    M5.Display.drawString("paperspecimen.local", cx, y);
    y += lineH;
    char ipStr[32];
    snprintf(ipStr, sizeof(ipStr), "(or %s)", ip.toString().c_str());
    M5.Display.drawString(ipStr, cx, y);

    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.display();
    M5.Display.waitDisplay();

    // Disable touch — interaction is via browser only
    M5.Touch.end();

    // Run server loop until timeout or user confirms
    wifiLastActivity = millis();
    unsigned long startMs = millis();
    wifiManagerDone = false;
    int lastDisplayedSecond = -1;
    int fullRefreshCounter = 0;

    Serial.printf("WiFi manager active — %ds inactivity timeout\n", WIFI_TIMEOUT_MS / 1000);
    while (!wifiManagerDone && (millis() - wifiLastActivity < WIFI_TIMEOUT_MS)) {
        dnsServer.processNextRequest();
        wifiServer.handleClient();

        // Update countdown on e-ink every second (based on last activity)
        int elapsed = (millis() - wifiLastActivity) / 1000;
        int remaining = (WIFI_TIMEOUT_MS / 1000) - elapsed;
        if (remaining < 0) remaining = 0;

        if (remaining != lastDisplayedSecond) {
            lastDisplayedSecond = remaining;
            int mins = remaining / 60;
            int secs = remaining % 60;
            char timerStr[48];
            snprintf(timerStr, sizeof(timerStr), "WiFi Font Manager - %dm %02ds", mins, secs);

            // epd_fastest every second, full refresh every 60s
            fullRefreshCounter++;
            bool doFullRefresh = (fullRefreshCounter >= 60);
            if (doFullRefresh) fullRefreshCounter = 0;

            if (doFullRefresh) {
                // Redraw entire screen for clean full refresh
                M5.Display.fillScreen(TFT_WHITE);
                M5.Display.setTextDatum(top_center);
                M5.Display.drawString("PaperSpecimen S3", cx, UI_PAD);
                M5.Display.setTextDatum(bottom_center);
                M5.Display.drawString(VERSION, cx, displayH - UI_PAD);
                M5.Display.setTextDatum(top_center);
                M5.Display.drawString(timerStr, cx, timerY);
                int ry = timerY + lineH + gapH;
                M5.Display.drawString("SSID:", cx, ry); ry += lineH;
                M5.Display.drawString(WIFI_SSID, cx, ry); ry += lineH + gapH;
                M5.Display.drawString("Password:", cx, ry); ry += lineH;
                M5.Display.drawString(WIFI_PASS, cx, ry); ry += lineH + gapH;
                M5.Display.drawString("Open in browser:", cx, ry); ry += lineH;
                M5.Display.drawString("paperspecimen.local", cx, ry); ry += lineH;
                M5.Display.drawString(ipStr, cx, ry);
                M5.Display.setEpdMode(epd_mode_t::epd_quality);
            } else {
                // Just update timer area
                M5.Display.fillRect(0, timerY, displayW, lineH, TFT_WHITE);
                M5.Display.setTextDatum(top_center);
                M5.Display.drawString(timerStr, cx, timerY);
                M5.Display.setEpdMode(epd_mode_t::epd_fastest);
            }
            M5.Display.display();
            M5.Display.waitDisplay();
        }

        delay(10);
    }
    if (wifiManagerDone) {
        Serial.println("WiFi manager: user confirmed or page timed out");
    } else {
        Serial.println("WiFi manager: 5 minute timeout reached");
    }

    // Cleanup
    Serial.println("WiFi Font Manager closing...");
    wifiServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    esp_wifi_deinit();

    // Restore CPU
    setCpuFrequencyMhz(80);

    Serial.println("WiFi off, restarting...");
    delay(500);
    ESP.restart();
}

// ---- Setup & Loop ----

void setup() {
    // Check debug mode from NVS early, before initializing serial
    // This avoids wasting power on serial in normal mode
    bool earlyDebug = false;
    {
        Preferences prefs;
        prefs.begin("ps3", true);
        bool wasSleeping = prefs.getBool("sleeping", false);
        prefs.end();
        // If device was sleeping, config exists — check it for debug mode
        // If not sleeping (first boot), we'll need serial for setup
        if (wasSleeping) {
            // We'll read debugMode from SD config later, but for serial init
            // we need a quick check. Use a dedicated NVS key.
            Preferences dbgPrefs;
            dbgPrefs.begin("ps3cfg", true);
            earlyDebug = dbgPrefs.getBool("debug", false);
            dbgPrefs.end();
        } else {
            earlyDebug = true; // first boot: enable serial for setup
        }
    }

    // Lower CPU frequency to save power (80MHz is sufficient for FreeType + e-ink)
    setCpuFrequencyMhz(80);

    auto cfg = M5.config();
    if (earlyDebug) {
        cfg.serial_baudrate = 115200;
    } else {
        cfg.serial_baudrate = 0; // don't initialize serial
    }
    cfg.clear_display = false;  // don't refresh EPD on init (preserves image from sleep)
    cfg.internal_imu  = false;  // disable BMI270 accelerometer/gyro (not used)
    cfg.internal_spk  = false;  // disable speaker init (not wired on PaperS3)
    cfg.internal_mic  = false;  // disable microphone (not present on PaperS3)
    cfg.led_brightness = 0;     // disable status LED
    M5.begin(cfg);

    // Disable WiFi and Bluetooth radio to save power
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_mem_release(ESP_BT_MODE_BTDM);

    if (earlyDebug) {
        delay(2000);
        Serial.println();
        Serial.println("*** PaperSpecimen S3 BOOT ***");
        Serial.printf("Version: %s\n", VERSION);
    }
    if (earlyDebug) Serial.printf("Board: %d\n", M5.getBoard());

    // Capture RTC time IMMEDIATELY after M5.begin, before any slow operations
    // (scanFonts with 100 fonts can take 5-10s, which would skew wake detection)
    {
        m5::rtc_time_t t;
        m5::rtc_date_t d;
        M5.Rtc.getTime(&t);
        M5.Rtc.getDate(&d);
        earlyBootRtcSec = t.hours * 3600 + t.minutes * 60 + t.seconds;
        Serial.printf("RTC at boot: %04d-%02d-%02d %02d:%02d:%02d\n",
                      d.year, d.month, d.date, t.hours, t.minutes, t.seconds);
    }

    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.setAutoDisplay(false);
    displayW = M5.Display.width();
    displayH = M5.Display.height();
    Serial.printf("Display: %d x %d\n", displayW, displayH);

    // Initialize outline colors
    colorOutline = M5.Display.color565(70, 70, 70);
    colorConstruct = M5.Display.color565(0, 0, 0);

    // ---- Low battery check (before anything else) ----
    // Average 5 readings to reduce ADC noise
    {
        uint32_t voltageSum = 0;
        for (int i = 0; i < 5; i++) {
            voltageSum += M5.Power.getBatteryVoltage();
            delay(10);
        }
        uint32_t voltage = voltageSum / 5;
        if (voltage >= 4200) batteryPct = 100.0;
        else if (voltage <= 3300) batteryPct = 0.0;
        else batteryPct = ((float)(voltage - 3300) / 900.0) * 100.0;

        Serial.printf("Battery: %dmV (%.1f%%)\n", voltage, batteryPct);

        if (batteryPct < 5.0) {
            Serial.println("LOW BATTERY — drawing icon and shutting down");

            // Clear NVS sleep flag so device won't try timer wake
            Preferences prefs;
            prefs.begin("ps3", false);
            prefs.putBool("sleeping", false);
            prefs.end();

            // Draw low battery icon centered on screen
            // Icon design from original PaperSpecimen: hollow rectangle + tip + thin bar
            M5.Display.fillScreen(TFT_WHITE);

            int iconW = 120, iconH = 60;
            int tipW = 10, tipH = 20;
            int iconX = (displayW - iconW - tipW) / 2;
            int iconY = (displayH - iconH) / 2;

            // Main body outline (black)
            M5.Display.fillRect(iconX, iconY, iconW, iconH, TFT_BLACK);
            // White interior (3px border)
            M5.Display.fillRect(iconX + 3, iconY + 3, iconW - 6, iconH - 6, TFT_WHITE);
            // Right tip (positive terminal)
            M5.Display.fillRect(iconX + iconW, iconY + (iconH - tipH) / 2, tipW, tipH, TFT_BLACK);
            // Thin gray bar on left (low charge indicator)
            uint16_t gray = M5.Display.color565(180, 180, 180);
            M5.Display.fillRect(iconX + 6, iconY + 6, 6, iconH - 12, gray);

            // Full refresh and shutdown
            M5.Display.setEpdMode(epd_mode_t::epd_quality);
            M5.Display.display();
            M5.Display.waitDisplay();

            Serial.println("Powering off (no timer)");
            Serial.flush();
            M5.Power.powerOff();
            esp_deep_sleep_start(); // fallback
        }
    }

    // Check NVS for flash font flag (set during splash/setup)
    {
        Preferences prefs;
        prefs.begin("ps3", true);
        fontsInFlash = prefs.getBool("fonts_flash", false);
        prefs.end();
    }

    // Initialize storage based on where fonts are
    if (fontsInFlash) {
        // Try flash first
        if (LittleFS.begin(false)) { // don't format
            flashInitialized = true;
            Serial.println("LittleFS: OK (fonts in flash)");
        } else {
            // Flash failed — fall back to SD
            Serial.println("LittleFS failed — falling back to SD");
            fontsInFlash = false;
        }
    }

    if (!fontsInFlash) {
        // Try SD first
        SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
        if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
            Serial.println("SD not available — trying flash as fallback...");
            // No SD: try LittleFS — may have fonts from before, or we'll extract embedded
            if (LittleFS.begin(true)) { // format if needed
                flashInitialized = true;
                // Check if flash has any fonts
                bool flashHasFonts = false;
                File testDir = LittleFS.open("/fonts");
                if (testDir && testDir.isDirectory()) {
                    File tf = testDir.openNextFile();
                    if (tf) { flashHasFonts = true; tf.close(); }
                    testDir.close();
                } else if (testDir) testDir.close();

                if (flashHasFonts) {
                    fontsInFlash = true;
                    Serial.println("SD not available — using flash fonts");
                } else {
                    // No fonts anywhere — extract embedded defaults
                    Serial.println("No SD, no flash fonts — extracting embedded defaults...");
                    LittleFS.mkdir("/fonts");
                    extractEmbeddedFonts();
                    fontsInFlash = true;
                    Serial.println("Embedded fonts extracted to flash");
                }
                // Update NVS
                Preferences prefs;
                prefs.begin("ps3", false);
                prefs.putBool("fonts_flash", true);
                prefs.end();
            } else {
                Serial.println("FATAL: No SD and LittleFS failed");
                M5.Display.fillScreen(TFT_WHITE);
                M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
                M5.Display.setFont(&fonts::Font0);
                M5.Display.setTextSize(2);
                M5.Display.setTextDatum(top_center);
                M5.Display.drawString("STORAGE ERROR", displayW / 2, displayH / 2);
                M5.Display.display();
                return;
            }
        } else {
            Serial.println("SD Card: OK");
        }
    }

    int count = scanFonts();
    if (count == 0) {
        Serial.println("NO FONTS FOUND");
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.setFont(&fonts::Font0);
        M5.Display.setTextSize(2);
        M5.Display.setTextDatum(top_center);
        M5.Display.drawString("NO FONTS FOUND", displayW / 2, displayH / 2);
        M5.Display.drawString("Add .ttf or .otf to /fonts", displayW / 2, displayH / 2 + 40);
        M5.Display.display();
        return;
    }
    Serial.printf("Found %d fonts\n", count);


    FT_Error err = FT_Init_FreeType(&ftLibrary);
    if (err) {
        Serial.printf("FT_Init_FreeType failed: 0x%02X\n", err);
        return;
    }
    Serial.println("FreeType initialized");
    // Manually register CFF modules for OTF support
    registerCFFModules(ftLibrary);

    // Use ESP32-S3 hardware RNG for true randomness
    // (analogRead(0) doesn't work on S3 — GPIO0 is not an ADC pin)
    randomSeed(esp_random());

    // Load or initialize config
    initDefaultConfig();
    loadConfig(); // load if exists, otherwise defaults stay
    debugMode = config.isDebugMode; // restore debug mode from persisted config
    applyRotation(); // apply flip if configured

    // Detect wakeup cause:
    // 0 = first boot / NVS clean (never slept)
    // 1 = timer wake (woke at expected time)
    // 2 = manual wake (button pressed before timer expired)
    int wakeReason = checkWakeReason();
    Serial.printf("Wakeup cause: %s\n",
                  wakeReason == 0 ? "FIRST BOOT" :
                  wakeReason == 1 ? "TIMER" : "MANUAL (button)");

    if (wakeReason == 1) {
        // ---- TIMER WAKE: new glyph, then back to sleep ----
        // Disable touch — not needed, saves power, prevents accidental input
        M5.Touch.end();
        Serial.println("Timer wake — touch disabled, rendering new content");

        // Restore last state (already read from NVS in checkWakeReason), then randomize if allowed
        {
            if (config.allowDifferentFont) {
                currentFontIndex = pickRandomEnabledFont();
            } else {
                currentFontIndex = nvsLastFont;
                ensureFontsEnabled();
                if (currentFontIndex >= fontCount || !config.fontEnabled[currentFontIndex]) {
                    for (int i = 0; i < fontCount; i++) {
                        if (config.fontEnabled[i]) { currentFontIndex = i; break; }
                    }
                }
            }
            if (config.allowDifferentMode) {
                currentViewMode = random(2) == 0 ? VIEW_BITMAP : VIEW_OUTLINE;
            } else {
                currentViewMode = (ViewMode)nvsLastMode;
            }
        }

        Serial.printf("Font: %s, Mode: %s\n",
                      fontNames[currentFontIndex].c_str(),
                      currentViewMode == VIEW_BITMAP ? "BITMAP" : "OUTLINE");

        if (loadFontFromStream(currentFontIndex)) {
            // NOTE: With FT_Open_Face stream, the font file must remain open
            // for FreeType to read glyphs. SD/LittleFS stays open until sleep.

            isFirstRender = true;

            // Find a non-blank glyph (trial fonts may have empty glyphs)
            uint32_t cp = findRandomGlyph();
            for (int attempt = 0; attempt < 5 && !isGlyphVisible(cp); attempt++) {
                Serial.printf("Timer wake: U+%04X is blank, retrying\n", cp);
                cp = findRandomGlyph();
            }
            Serial.printf("Timer wake glyph: U+%04X\n", cp);
            if (currentViewMode == VIEW_OUTLINE)
                drawGlyphOutline(cp);
            else
                drawGlyph(cp);
            logBatteryDebug(cp, "timer");
            M5.Display.waitDisplay();
            Serial.println("Timer wake: display done");
        } else {
            Serial.println("ERROR: loadFontFromStream FAILED on timer wake!");
        }
        goToSleepAnchored();

    } else if (wakeReason == 2) {
        // ---- MANUAL WAKE (button): restore state + interactive mode ----
        // Display still shows last glyph (clear_display=false), just restore state
        Serial.println("Manual wake — restoring state, entering interactive mode");
        logBatteryBlankLine(); // separate from previous timer interval

        // Sanitize debug timer values
        if (config.wakeIntervalMinutes < 5) config.wakeIntervalMinutes = 15;

        // Restore last font/mode (already read from NVS in checkWakeReason)
        {
            currentFontIndex = nvsLastFont;
            currentViewMode = (ViewMode)nvsLastMode;

            if (currentFontIndex >= fontCount || !config.fontEnabled[currentFontIndex]) {
                for (int i = 0; i < fontCount; i++) {
                    if (config.fontEnabled[i]) { currentFontIndex = i; break; }
                }
            }
        }

        Serial.printf("Restored: Font=%s, Mode=%s\n",
                      fontNames[currentFontIndex].c_str(),
                      currentViewMode == VIEW_BITMAP ? "BITMAP" : "OUTLINE");

        // Load font into memory for interactive use (touch navigation)
        loadFontFromStream(currentFontIndex);
        // Don't render a new glyph — display still shows the last one
        // Fall through to loop() for interactive touch + inactivity sleep

    } else {
        // ---- FIRST BOOT: splash + setup + first glyph ----
        Serial.println("First boot — setup screen");

        // Ensure SD is available for splash font scan (even if fonts were in flash before)
        if (fontsInFlash) {
            SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
            if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
                Serial.println("SD not available — using flash fonts only");
            }
        }

        runSplashAndSetup();
        // Fall through to loop() for interactive mode + inactivity sleep
    }

    Serial.println("Interactive mode. Inactivity timeout: 60s. Hold center 10s: setup.");
}

// Inactivity timer for sleep
static unsigned long lastInteraction = 0;
#define INACTIVITY_TIMEOUT_MS 40000
// Hold detection for setup
#define HOLD_FOR_SETUP_MS 5000

void loop() {
    M5.update();

    // Initialize interaction timer on first loop iteration
    if (lastInteraction == 0) lastInteraction = millis();

    // Inactivity sleep: 60s without touch → go to sleep
    if (millis() - lastInteraction >= INACTIVITY_TIMEOUT_MS) {
        Serial.println("Inactivity timeout — going to sleep");
        goToSleep();
    }

    auto touch = M5.Touch.getDetail();

    // Hold detection: center zone (180-360, 90-870)
    // 1s+: show filling circle feedback, 5s+: enter setup
    #define HOLD_FEEDBACK_START_MS 1000
    #define HOLD_CIRCLE_MAX_R 100
    static int lastCircleR = 0;

    uint32_t holdDuration = millis() - touch.base_msec;
    if (touch.isPressed()) {
        int x = touch.x;
        int y = touch.y;
        bool inCenterZone = (x >= 180 && x < 360 && y >= 90 && y < displayH - 90);

        if (inCenterZone && holdDuration >= HOLD_FEEDBACK_START_MS) {
            // Calculate circle progress: 0 at 1s, full at 5s
            float progress = (float)(holdDuration - HOLD_FEEDBACK_START_MS) /
                            (float)(HOLD_FOR_SETUP_MS - HOLD_FEEDBACK_START_MS);
            if (progress > 1.0f) progress = 1.0f;
            int circleR = (int)(progress * HOLD_CIRCLE_MAX_R);

            // Only redraw if radius changed (avoid excessive redraws)
            if (circleR != lastCircleR) {
                wakeDisplay(); // wake display for circle feedback
                int cx = displayW / 2;
                int cy = displayH / 2;
                // Erase previous circle area
                M5.Display.fillCircle(cx, cy, HOLD_CIRCLE_MAX_R + 2, TFT_WHITE);
                // Draw outline ring
                M5.Display.fillSmoothCircle(cx, cy, HOLD_CIRCLE_MAX_R, colorOutline);
                M5.Display.fillSmoothCircle(cx, cy, HOLD_CIRCLE_MAX_R - 2, TFT_WHITE);
                // Draw filled portion
                if (circleR > 0) {
                    M5.Display.fillSmoothCircle(cx, cy, circleR, colorOutline);
                }
                M5.Display.setEpdMode(epd_mode_t::epd_fastest);
                M5.Display.display();
                lastCircleR = circleR;
            }
        }

        if (inCenterZone && holdDuration >= HOLD_FOR_SETUP_MS) {
            lastCircleR = 0;
            // Re-enable serial for setup (may have been disabled in normal mode)
            Serial.begin(115200);
            delay(500);
            Serial.printf("Long hold (%dms) — full restart\n", holdDuration);
            M5.Display.waitDisplay();
            // Close font file and FreeType before setup (prevents "open FD" errors)
            if (ftFace) { FT_Done_Face(ftFace); ftFace = nullptr; }
            if (fontFile) { fontFile.close(); }
            if (ftLibrary) { FT_Done_FreeType(ftLibrary); ftLibrary = nullptr; }
            runSplashAndSetup();
            lastInteraction = millis();
            return;
        }
    } else {
        // Touch released — redraw glyph if circle was showing
        if (lastCircleR > 0) {
            lastCircleR = 0;
            M5.Display.waitDisplay();
            // Redraw current glyph to restore the area under the circle
            if (currentViewMode == VIEW_OUTLINE)
                drawGlyphOutline(currentCodepoint);
            else
                drawGlyph(currentCodepoint);
        }
    }

    // Any touch resets inactivity timer
    if (touch.isPressed() || touch.wasPressed()) {
        lastInteraction = millis();
    }

    // Short tap actions (on release, only if held < 5s)
    // Touch zones (540x960 portrait):
    //   Top strip    (y < 90):            toggle bitmap/outline, keep glyph+font
    //   Bottom strip (y >= 870):          toggle bitmap/outline, keep glyph+font
    //   Left column  (90<=y<870, x<180):  prev font, keep glyph+mode
    //   Center       (90<=y<870, 180<=x<360): random glyph, keep font+mode
    //   Right column (90<=y<870, x>=360): next font, keep glyph+mode
    #define TOUCH_STRIP_H  90
    #define TOUCH_COL_W    180

    if (touch.wasReleased() && holdDuration < HOLD_FOR_SETUP_MS) {
        M5.Display.waitDisplay();

        int x = touch.x;
        int y = touch.y;

        Serial.printf("Tap at (%d, %d) held=%dms\n", x, y, holdDuration);

        if (y < TOUCH_STRIP_H || y >= displayH - TOUCH_STRIP_H) {
            currentViewMode = (currentViewMode == VIEW_BITMAP) ? VIEW_OUTLINE : VIEW_BITMAP;
            Serial.printf("Toggle mode: %s\n", currentViewMode == VIEW_BITMAP ? "BITMAP" : "OUTLINE");
            if (currentViewMode == VIEW_OUTLINE)
                drawGlyphOutline(currentCodepoint);
            else
                drawGlyph(currentCodepoint);
        }
        else if (x < TOUCH_COL_W) {
            int start = currentFontIndex;
            do {
                currentFontIndex--;
                if (currentFontIndex < 0) currentFontIndex = fontCount - 1;
            } while (!config.fontEnabled[currentFontIndex] && currentFontIndex != start);
            Serial.printf("Prev font: %s\n", fontNames[currentFontIndex].c_str());
            if (loadFontFromStream(currentFontIndex)) {
                if (currentViewMode == VIEW_OUTLINE)
                    drawGlyphOutline(currentCodepoint);
                else
                    drawGlyph(currentCodepoint);
            }
        } else if (x >= TOUCH_COL_W * 2) {
            int start = currentFontIndex;
            do {
                currentFontIndex++;
                if (currentFontIndex >= fontCount) currentFontIndex = 0;
            } while (!config.fontEnabled[currentFontIndex] && currentFontIndex != start);
            Serial.printf("Next font: %s\n", fontNames[currentFontIndex].c_str());
            if (loadFontFromStream(currentFontIndex)) {
                if (currentViewMode == VIEW_OUTLINE)
                    drawGlyphOutline(currentCodepoint);
                else
                    drawGlyph(currentCodepoint);
            }
        } else {
            uint32_t cp = findRandomGlyph();
            Serial.printf("Random glyph: U+%04X\n", cp);
            if (currentViewMode == VIEW_OUTLINE)
                drawGlyphOutline(cp);
            else
                drawGlyph(cp);
        }
    }

    // Timed full refresh (anti-ghosting while in interactive mode)
    if (hasPartialSinceLastFull && ftFace) {
        unsigned long timeSinceFirstPartial = millis() - firstPartialAfterFullTime;
        unsigned long timeSinceLastFull = millis() - lastFullRefreshTime;

        if (timeSinceFirstPartial >= FULL_REFRESH_TIMEOUT_MS &&
            timeSinceLastFull >= FULL_REFRESH_TIMEOUT_MS) {
            // Wait for any pending refresh to complete before redrawing
            M5.Display.waitDisplay();
            Serial.println("Timed full refresh");
            partialRefreshCount = MAX_PARTIAL_BEFORE_FULL;
            if (currentViewMode == VIEW_OUTLINE) {
                drawGlyphOutline(currentCodepoint);
            } else {
                drawGlyph(currentCodepoint);
            }
        }
    }

    // Delayed display sleep: 3s after last FULL refresh with no input, sleep the controller
    // Partial refreshes don't reset the timer — display stays awake for the timed full refresh
    if (!displaySleeping && lastFullRefreshDone > 0 &&
        millis() - lastFullRefreshDone >= DISPLAY_SLEEP_DELAY_MS) {
        sleepDisplay();
    }

    delay(20); // ~50Hz touch polling — saves CPU cycles vs 100Hz, still responsive
}
