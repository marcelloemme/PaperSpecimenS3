#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_bt.h>

// FreeType headers (provided by OpenFontRender)
#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_BBOX_H

// PaperSpecimen S3 - v0.4.0 (Config UI + Unicode Ranges)
static const char* VERSION = "v0.4.0";

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
#define CONFIG_FILE "/paperspecimen.cfg"

// Max fonts (used by config and font list)
#define MAX_FONTS 32

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
    bool allowDifferentMode;           // Random mode on wake
    bool isDebugMode;                  // Debug mode (persisted across wakes)
    bool fontEnabled[MAX_FONTS];       // Per-font enable
    bool rangeEnabled[28];             // Per-range enable
};

static AppConfig config;

// ---- Config save/load ----

void initDefaultConfig() {
    config.wakeIntervalMinutes = 15;
    config.allowDifferentFont = true;
    config.isDebugMode = false;
    config.allowDifferentMode = true;
    for (int i = 0; i < MAX_FONTS; i++) config.fontEnabled[i] = true;
    for (int i = 0; i < NUM_GLYPH_RANGES; i++) config.rangeEnabled[i] = (i < 6);
}

bool loadConfig() {
    if (!SD.exists(CONFIG_FILE)) return false;

    File f = SD.open(CONFIG_FILE, FILE_READ);
    if (!f) return false;

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

    File f = SD.open(CONFIG_FILE, FILE_WRITE);
    if (!f) { Serial.println("ERROR: Can't write config"); return false; }

    f.println(config.wakeIntervalMinutes);
    f.println(config.allowDifferentFont ? "1" : "0");
    f.println(config.allowDifferentMode ? "1" : "0");
    f.println(config.isDebugMode ? "1" : "0");

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
static uint8_t* fontBuffer = nullptr;
static size_t fontBufferSize = 0;

// Display
static int displayW, displayH;

// View mode
enum ViewMode { VIEW_BITMAP, VIEW_OUTLINE };
static ViewMode currentViewMode = VIEW_BITMAP;

// Debug mode (activated by 2+ taps during splash)
static bool debugMode = false;
static float batteryPct = 100.0;  // updated at every boot

// ---- Outline data structures ----

#define MAX_OUTLINE_POINTS 200
#define MAX_OUTLINE_SEGMENTS 200

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
    // Skip macOS resource fork files (._filename)
    if (n.startsWith("._")) return false;
    n.toLowerCase();
    return n.endsWith(".ttf") || n.endsWith(".otf");
}

int scanFonts() {
    File dir = SD.open("/fonts");
    if (!dir || !dir.isDirectory()) {
        Serial.println("ERROR: /fonts directory not found");
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

// ---- FreeType ----

bool loadFontToMemory(int index) {
    if (ftFace) { FT_Done_Face(ftFace); ftFace = nullptr; }
    if (fontBuffer) { free(fontBuffer); fontBuffer = nullptr; }

    // Wait for any pending display refresh before accessing SD
    M5.Display.waitDisplay();

    File f = SD.open(fontPaths[index], FILE_READ);
    if (!f) {
        Serial.printf("Failed to open: %s\n", fontPaths[index].c_str());
        return false;
    }

    fontBufferSize = f.size();
    fontBuffer = (uint8_t*)ps_malloc(fontBufferSize);
    if (!fontBuffer) {
        Serial.printf("PSRAM alloc failed for %u bytes\n", fontBufferSize);
        f.close();
        return false;
    }

    f.read(fontBuffer, fontBufferSize);
    f.close();

    FT_Error err = FT_New_Memory_Face(ftLibrary, fontBuffer, fontBufferSize, 0, &ftFace);
    if (err) {
        Serial.printf("FT_New_Memory_Face failed: 0x%02X\n", err);
        free(fontBuffer); fontBuffer = nullptr;
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
    int totalWidth = 0;
    int maxAscent = 0;
    int maxDescent = 0;
    for (const char* p = text; *p; p++) {
        FT_UInt idx = FT_Get_Char_Index(ftFace, (uint8_t)*p);
        if (idx == 0) idx = FT_Get_Char_Index(ftFace, '?');
        if (idx == 0) continue;

        FT_Load_Glyph(ftFace, idx, FT_LOAD_DEFAULT);
        totalWidth += ftFace->glyph->advance.x >> 6;

        FT_Load_Glyph(ftFace, idx, FT_LOAD_RENDER);
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

    // Second pass: render
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

        for (unsigned int row = 0; row < bmp->rows; row++) {
            for (unsigned int col = 0; col < bmp->width; col++) {
                uint8_t alpha = bmp->buffer[row * bmp->pitch + col];
                if (alpha > 0) {
                    uint8_t gray = 255 - alpha;
                    int px = bx + col;
                    int py = by + row;
                    if (px >= 0 && px < displayW && py >= 0 && py < displayH) {
                        M5.Display.drawPixel(px, py,
                            M5.Display.color565(gray, gray, gray));
                    }
                }
            }
        }

        penX += slot->advance.x >> 6;
    }
}

// Forward declarations
void refreshDisplay();
uint32_t findRandomGlyph();

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

// ---- Outline rendering ----

void drawGlyphOutline(uint32_t charcode) {
    if (!ftFace) return;

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

    // Draw points
    for (int i = 0; i < g_num_points; i++) {
        OutlinePoint& pt = g_outline_points[i];
        if (pt.is_control) {
            // Off-curve: hollow circle (black outer, white inner)
            M5.Display.fillCircle((int)pt.x, (int)pt.y, 4, colorConstruct);
            M5.Display.fillCircle((int)pt.x, (int)pt.y, 3, TFT_WHITE);
        } else {
            // On-curve: filled black circle
            M5.Display.fillCircle((int)pt.x, (int)pt.y, 4, colorConstruct);
        }
    }

    // Labels (shorten font name if needed to fit display width)
    int labelMaxW = displayW - 2 * UI_PAD;
    String displayName = shortenFTText(fontNames[currentFontIndex], labelMaxW, LABEL_PIXEL_SIZE);
    drawFTString(displayName.c_str(),
                 displayW / 2, UI_PAD, LABEL_PIXEL_SIZE, false);

    char cpBuf[16];
    snprintf(cpBuf, sizeof(cpBuf), "U+%04X", charcode);
    drawFTString(cpBuf, displayW / 2, displayH - UI_PAD, LABEL_PIXEL_SIZE, true);

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

    // Draw glyph bitmap with grayscale anti-aliasing
    for (unsigned int row = 0; row < bmp->rows; row++) {
        for (unsigned int col = 0; col < bmp->width; col++) {
            uint8_t alpha = bmp->buffer[row * bmp->pitch + col];
            if (alpha > 0) {
                uint8_t gray = 255 - alpha;
                int px = drawX + col;
                int py = drawY + row;
                if (px >= 0 && px < displayW && py >= 0 && py < displayH) {
                    M5.Display.drawPixel(px, py,
                        M5.Display.color565(gray, gray, gray));
                }
            }
        }
    }

    // Draw labels using the current font at 24px (like original PaperSpecimen)
    // Font name at top (shorten if needed)
    int labelMaxW = displayW - 2 * UI_PAD;
    String displayName = shortenFTText(fontNames[currentFontIndex], labelMaxW, LABEL_PIXEL_SIZE);
    drawFTString(displayName.c_str(),
                 displayW / 2, UI_PAD, LABEL_PIXEL_SIZE, false);

    // Codepoint at bottom
    char cpBuf[16];
    snprintf(cpBuf, sizeof(cpBuf), "U+%04X", charcode);
    drawFTString(cpBuf, displayW / 2, displayH - UI_PAD, LABEL_PIXEL_SIZE, true);

    // Refresh with smart management
    refreshDisplay();
    currentCodepoint = charcode;
}

// ---- Setup UI ----

// UI layout (960x540 landscape, single column, Font0 textSize 2)
// 20px uniform spacing between all groups, 20px top/bottom padding
#define UI_LINE_H    36  // line height for items (text is 16px, rest is touch area)
#define UI_LEFT      30

// Setup refresh tracking (reuses same constants as main refresh logic)
static bool setupFirstRender = true;
static int setupPartialCount = 0;
static unsigned long setupLastFullTime = 0;
static unsigned long setupFirstPartialTime = 0;
static bool setupHasPartial = false;

// Common setup for all UI screens
void uiBeginScreen(const char* title) {
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::Font0);

    // Header
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(title, displayW / 2, UI_PAD);

    // Footer
    M5.Display.setTextDatum(bottom_center);
    M5.Display.drawString(VERSION, displayW / 2, displayH - UI_PAD);

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

// Track Y positions of each item for hit testing
#define MAX_UI_ITEMS 20
struct UiItem {
    int y;
    int h;
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
#define ID_FONT_SELALL   40
#define ID_FONT_BASE     100   // 100 + fontIndex
#define ID_FONT_PREV     200
#define ID_FONT_NEXT     201

void addUiItem(int y, int h, int id) {
    if (uiItemCount < MAX_UI_ITEMS) {
        uiItems[uiItemCount++] = {y, h, id};
    }
}

int hitTestUi(int ty) {
    for (int i = 0; i < uiItemCount; i++) {
        if (ty >= uiItems[i].y && ty < uiItems[i].y + uiItems[i].h)
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

    // Fonts header (indented label + "> Deselect all" on right, measured as full line)
    w = uiTextWidth("Fonts:  > Deselect all"); if (w > maxW) maxW = w;

    // Calculate pagination based on available vertical space
    // Usable area between title and version
    int areaTop0 = UI_PAD + 16 + UI_PAD;
    int areaBot0 = displayH - UI_PAD - 16 - UI_PAD;
    int areaH0 = areaBot0 - areaTop0;
    // Fixed lines: timer(1+3) + standby(1+2) + fonts_header(1) + unicode(1) + confirm(1) = 10
    // Fixed seps: 3 (after timer, after standby, after fonts/before unicode)
    int fixedH = 10 * UI_LINE_H + 3 * UI_PAD;
    int spaceForFonts = areaH0 - fixedH;
    int maxFontLines = spaceForFonts / UI_LINE_H;

    // Pagination logic: if all fonts fit, show all. Otherwise show (maxFontLines-1) + nav line.
    int fontsPerPage;
    int totalPages;
    if (fontCount <= maxFontLines) {
        fontsPerPage = fontCount;
        totalPages = 1;
    } else {
        fontsPerPage = maxFontLines - 1; // reserve 1 line for nav
        if (fontsPerPage < 2) fontsPerPage = 2;
        totalPages = (fontCount + fontsPerPage - 1) / fontsPerPage;
    }
    if (totalPages < 1) totalPages = 1;
    if (fontPage >= totalPages) fontPage = totalPages - 1;

    int startFont = fontPage * fontsPerPage;
    int endFont = startFont + fontsPerPage;
    if (endFont > fontCount) endFont = fontCount;

    // Measure font names (shorten if needed to fit screen with margins)
    int maxTextW = displayW - 2 * UI_LEFT; // max pixel width for any text line
    for (int i = startFont; i < endFont; i++) {
        snprintf(buf, sizeof(buf), "(*) %s", fontNames[i].c_str());
        String s = shortenText(String(buf), maxTextW);
        w = uiTextWidth(s.c_str()); if (w > maxW) maxW = w;
    }

    // Calculate left X to center the whole block
    int leftX = uiGroupX(maxW);
    if (leftX < UI_LEFT) leftX = UI_LEFT;

    // -- Calculate total content height for vertical centering --
    // Lines: timer label(1) + 3 radios + standby label(1) + 2 checkboxes
    //        + fonts header(1) + fonts shown + unicode(1) + confirm(1)
    int contentLines = 4 + 3 + 1 + (endFont - startFont) + 2;
    int contentSeps = 3; // after timer, after standby, after fonts
    int navLineH = (totalPages > 1) ? UI_LINE_H : 0;
    int totalContentH = contentLines * UI_LINE_H + contentSeps * UI_PAD + navLineH;

    // Usable area: between title+pad and version+pad
    int areaTop = UI_PAD + 16 + UI_PAD;  // after title
    int areaBot = displayH - UI_PAD - 16 - UI_PAD; // before version
    int areaH = areaBot - areaTop;

    int startY = areaTop + (areaH - totalContentH) / 2;
    if (startY < areaTop) startY = areaTop;

    // -- Pass 2: draw everything --
    uiBeginScreen("PaperSpecimen S3 Setup");
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

    // Fonts header (label indented)
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
        addUiItem(y, UI_LINE_H, ID_FONT_PREV);
        addUiItem(y, UI_LINE_H, ID_FONT_NEXT);
        M5.Display.setTextDatum(top_left);
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
    M5.Display.drawString("Confirm", leftX + indent, y);
    addUiItem(y, UI_LINE_H, ID_CONFIRM);

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
    int rangesOnPage = endR - startR;
    int contentLines = 1 + rangesOnPage; // back/select line + range checkboxes
    int contentSeps = 1; // after back/select line
    int navLineH = (totalPages > 1) ? (UI_PAD + UI_LINE_H) : 0; // gap + nav line
    int totalContentH = contentLines * UI_LINE_H + contentSeps * UI_PAD + navLineH;

    int areaTop = UI_PAD + 16 + UI_PAD;
    int areaBot = displayH - UI_PAD - 16 - UI_PAD;
    int areaH = areaBot - areaTop;

    int startY = areaTop + (areaH - totalContentH) / 2;
    if (startY < areaTop) startY = areaTop;

    // -- Pass 2: draw --
    uiBeginScreen("Unicode Ranges");
    M5.Display.setTextDatum(top_left);

    int y = startY;

    // Back (left-aligned) + Select/Deselect (right-aligned on same line)
    M5.Display.drawString("> Back", leftX, y);
    addUiItem(y, UI_LINE_H, ID_RANGE_BACK);

    bool allRanges = true;
    for (int i = 0; i < NUM_GLYPH_RANGES; i++)
        if (!config.rangeEnabled[i]) { allRanges = false; break; }
    snprintf(buf, sizeof(buf), "> %s all", allRanges ? "Deselect" : "Select");
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(buf, displayW - leftX, y);
    addUiItem(y, UI_LINE_H, ID_RANGE_SELALL);
    M5.Display.setTextDatum(top_left);
    y += UI_LINE_H + UI_PAD;

    // Range checkboxes
    for (int ri = startR; ri < endR; ri++) {
        snprintf(buf, sizeof(buf), "%s %s",
                 config.rangeEnabled[ri] ? "(*)" : "( )",
                 glyphRanges[ri].name);
        M5.Display.drawString(buf, leftX, y);
        addUiItem(y, UI_LINE_H, ID_RANGE_BASE + ri);
        y += UI_LINE_H;
    }

    // Page nav
    if (totalPages > 1) {
        y += UI_PAD;
        if (rangePage > 0) {
            M5.Display.drawString("<<<", leftX, y);
        }
        M5.Display.setTextDatum(top_center);
        snprintf(buf, sizeof(buf), "Page %d/%d", rangePage + 1, totalPages);
        M5.Display.drawString(buf, displayW / 2, y);
        if (rangePage < totalPages - 1) {
            M5.Display.setTextDatum(top_right);
            M5.Display.drawString(">>>", displayW - leftX, y);
        }
        addUiItem(y, UI_LINE_H, ID_RANGE_PREV);
        addUiItem(y, UI_LINE_H, ID_RANGE_NEXT);
        M5.Display.setTextDatum(top_left);
    }

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
        int id = hitTestUi(ty);
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

                int rid = hitTestUi(rt.y);
                int rtx = rt.x;
                bool rRedraw = true;

                if (rid == ID_RANGE_BACK) {
                    if (rtx < displayW / 2) { inRanges = false; rRedraw = false; }
                    else {
                        // Select/Deselect all (right half of that line)
                        bool all = true;
                        for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                            if (!config.rangeEnabled[i]) { all = false; break; }
                        for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                            config.rangeEnabled[i] = !all;
                    }
                }
                else if (rid == ID_RANGE_SELALL) {
                    if (rtx >= displayW / 2) {
                        bool all = true;
                        for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                            if (!config.rangeEnabled[i]) { all = false; break; }
                        for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                            config.rangeEnabled[i] = !all;
                    } else {
                        inRanges = false; rRedraw = false; // Back
                    }
                }
                else if (rid >= ID_RANGE_BASE && rid < ID_RANGE_PREV) {
                    int ri = rid - ID_RANGE_BASE;
                    config.rangeEnabled[ri] = !config.rangeEnabled[ri];
                    bool any = false;
                    for (int i = 0; i < NUM_GLYPH_RANGES; i++)
                        if (config.rangeEnabled[i]) { any = true; break; }
                    if (!any) config.rangeEnabled[ri] = true;
                }
                else if (rid == ID_RANGE_PREV || rid == ID_RANGE_NEXT) {
                    int totalPages = (NUM_GLYPH_RANGES + RANGES_PER_PAGE - 1) / RANGES_PER_PAGE;
                    if (rtx < displayW / 3 && rangePage > 0) rangePage--;
                    else if (rtx > displayW * 2 / 3 && rangePage < totalPages - 1) rangePage++;
                    else rRedraw = false;
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
//   - Close match (within 30s) = timer wake
//   - Far off (button pressed early) = manual wake

// Convert RTC date+time to a flat minute count for easy comparison
// (only needs to be consistent within a day, not absolute)
uint32_t rtcToMinutes() {
    auto dt = M5.Rtc.getDateTime();
    return (uint32_t)dt.time.hours * 60 + dt.time.minutes;
}

uint32_t rtcToSeconds() {
    auto dt = M5.Rtc.getDateTime();
    return (uint32_t)dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
}

// saveExpectedWakeTime is now inline in doSleepAt()

// Returns: 0 = not sleeping (cold boot), 1 = timer wake, 2 = manual wake (button)
// Also stores the expected wake time in lastExpectedWakeSec for anchored sleep calc
static uint32_t lastExpectedWakeSec = 0;

int checkWakeReason() {
    Preferences prefs;
    prefs.begin("ps3", false);
    bool wasSleeping = prefs.getBool("sleeping", false);
    if (!wasSleeping) {
        prefs.end();
        return 0; // never went to sleep (first boot, or NVS cleared)
    }

    lastExpectedWakeSec = prefs.getUInt("wake_sec", 0);
    // Clear flag
    prefs.putBool("sleeping", false);
    prefs.end();

    uint32_t nowSec = rtcToSeconds();

    // Calculate difference handling midnight wrap
    int32_t diff = (int32_t)nowSec - (int32_t)lastExpectedWakeSec;
    if (diff > 43200) diff -= 86400;
    if (diff < -43200) diff += 86400;

    // RTC alarm fires at exact HH:MM:00, boot takes ~2-3s, so timer wake
    // diff is always +2-5s. Any wake more than 5s off is a manual button press.
    #define WAKE_TOLERANCE_S 5
    int result;
    if (abs(diff) <= WAKE_TOLERANCE_S) {
        result = 1; // timer wake
    } else {
        result = 2; // manual wake (button pressed)
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
}

// Append an empty line to the battery log (separates manual wake sessions)
void logBatteryBlankLine() {
    if (!debugMode) return;
    File logf = SD.open("/battery_log.txt", FILE_APPEND);
    if (logf) {
        logf.println();
        logf.close();
    }
}

void doSleepAt(uint32_t wakeTargetSec) {
    int wakeH = wakeTargetSec / 3600;
    int wakeM = (wakeTargetSec % 3600) / 60;

    Serial.printf("Sleep target: wake at %02d:%02d:00\n", wakeH, wakeM);

    // Wait for display refresh to finish
    M5.Display.waitDisplay();

    // Clean up FreeType
    if (ftFace) { FT_Done_Face(ftFace); ftFace = nullptr; }
    if (fontBuffer) { free(fontBuffer); fontBuffer = nullptr; }
    if (ftLibrary) { FT_Done_FreeType(ftLibrary); ftLibrary = nullptr; }

    // Cleanly close SD card
    SD.end();

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
        // Save current font/mode so timer wake can preserve them if allow* is off
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

// Pick a random enabled font index
int pickRandomEnabledFont() {
    // Count enabled fonts
    int enabled[MAX_FONTS];
    int count = 0;
    for (int i = 0; i < fontCount; i++) {
        if (config.fontEnabled[i]) enabled[count++] = i;
    }
    if (count == 0) return 0;
    return enabled[random(count)];
}

// ---- Splash + Setup sequence (used at first boot and on long hold) ----

// QR Code bitmap for GitHub repository
// URL: https://github.com/marcelloemme/PaperSpecimen
// Size: 29x29 modules (Version 3 QR Code)
const uint8_t QR_SIZE = 29;
const uint8_t qrcode_data[] PROGMEM = {
    0b11111110, 0b01000010, 0b10111011, 0b11111000,  // Row 0
    0b10000010, 0b11011101, 0b11100010, 0b00001000,  // Row 1
    0b10111010, 0b01110001, 0b10010010, 0b11101000,  // Row 2
    0b10111010, 0b11010011, 0b01001010, 0b11101000,  // Row 3
    0b10111010, 0b00101010, 0b10111010, 0b11101000,  // Row 4
    0b10000010, 0b10100101, 0b00001010, 0b00001000,  // Row 5
    0b11111110, 0b10101010, 0b10101011, 0b11111000,  // Row 6
    0b00000000, 0b00001011, 0b00000000, 0b00000000,  // Row 7
    0b11111011, 0b11101111, 0b11000101, 0b01010000,  // Row 8
    0b11100001, 0b11000010, 0b11111011, 0b10001000,  // Row 9
    0b11110110, 0b11011001, 0b10001000, 0b10000000,  // Row 10
    0b10011100, 0b11110001, 0b00101000, 0b01010000,  // Row 11
    0b10110110, 0b11010111, 0b01010000, 0b01100000,  // Row 12
    0b00110100, 0b00101000, 0b11111111, 0b10001000,  // Row 13
    0b10100011, 0b11100011, 0b00001100, 0b11100000,  // Row 14
    0b00111001, 0b10001011, 0b00111111, 0b10010000,  // Row 15
    0b01000111, 0b00101111, 0b11010101, 0b01100000,  // Row 16
    0b11010101, 0b11100001, 0b11110111, 0b10101000,  // Row 17
    0b10100110, 0b11111101, 0b11001011, 0b10100000,  // Row 18
    0b10110101, 0b00010010, 0b10001110, 0b00010000,  // Row 19
    0b10001110, 0b01111110, 0b01011111, 0b10111000,  // Row 20
    0b00000000, 0b10101100, 0b00101000, 0b11111000,  // Row 21
    0b11111110, 0b11001001, 0b10011010, 0b11100000,  // Row 22
    0b10000010, 0b00010011, 0b10011000, 0b10000000,  // Row 23
    0b10111010, 0b10111111, 0b01001111, 0b10101000,  // Row 24
    0b10111010, 0b11010100, 0b11111000, 0b01111000,  // Row 25
    0b10111010, 0b11100011, 0b10011111, 0b11110000,  // Row 26
    0b10000010, 0b11000011, 0b10001101, 0b01010000,  // Row 27
    0b11111110, 0b11101110, 0b01010011, 0b10100000   // Row 28
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
    if (tapCount >= 2) {
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

    // Run setup screen
    setupFirstRender = true;
    runSetupScreen();

    // After setup, render first glyph
    currentFontIndex = 0;
    for (int i = 0; i < fontCount; i++) {
        if (config.fontEnabled[i]) { currentFontIndex = i; break; }
    }

    if (loadFontToMemory(currentFontIndex)) {
        isFirstRender = true;
        uint32_t cp = findRandomGlyph();
        drawGlyph(cp);
        M5.Display.waitDisplay();
    }
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
    Serial.printf("Board: %d\n", M5.getBoard());

    // Log RTC time immediately after M5.begin for wake debug
    {
        m5::rtc_time_t t;
        m5::rtc_date_t d;
        M5.Rtc.getTime(&t);
        M5.Rtc.getDate(&d);
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

    // SD Card
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        Serial.println("SD CARD ERROR");
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.setFont(&fonts::Font0);
        M5.Display.setTextSize(2);
        M5.Display.setTextDatum(top_center);
        M5.Display.drawString("SD CARD ERROR", displayW / 2, displayH / 2);
        M5.Display.display();
        return;
    }
    Serial.println("SD Card: OK");

    int count = scanFonts();
    if (count == 0) {
        Serial.println("NO FONTS FOUND");
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
        M5.Display.setFont(&fonts::Font0);
        M5.Display.setTextSize(2);
        M5.Display.setTextDatum(top_center);
        M5.Display.drawString("NO FONTS FOUND", displayW / 2, displayH / 2);
        M5.Display.drawString("Add .ttf to /fonts", displayW / 2, displayH / 2 + 40);
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

    // Use ESP32-S3 hardware RNG for true randomness
    // (analogRead(0) doesn't work on S3 — GPIO0 is not an ADC pin)
    randomSeed(esp_random());

    // Load or initialize config
    initDefaultConfig();
    loadConfig(); // load if exists, otherwise defaults stay
    debugMode = config.isDebugMode; // restore debug mode from persisted config

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
        Serial.println("Timer wake — touch disabled, rendering new glyph");

        // Restore last font/mode from NVS, then randomize only if allowed
        {
            Preferences prefs;
            prefs.begin("ps3", true);
            int lastFont = prefs.getInt("last_font", 0);
            int lastMode = prefs.getInt("last_mode", VIEW_BITMAP);
            prefs.end();

            if (config.allowDifferentFont) {
                currentFontIndex = pickRandomEnabledFont();
            } else {
                // Keep the same font that was showing when device went to sleep
                currentFontIndex = lastFont;
                if (currentFontIndex >= fontCount || !config.fontEnabled[currentFontIndex]) {
                    // Fallback: pick first enabled font
                    for (int i = 0; i < fontCount; i++) {
                        if (config.fontEnabled[i]) { currentFontIndex = i; break; }
                    }
                }
            }
            if (config.allowDifferentMode) {
                currentViewMode = random(2) == 0 ? VIEW_BITMAP : VIEW_OUTLINE;
            } else {
                currentViewMode = (ViewMode)lastMode;
            }
        }

        Serial.printf("Font: %s, Mode: %s\n",
                      fontNames[currentFontIndex].c_str(),
                      currentViewMode == VIEW_BITMAP ? "BITMAP" : "OUTLINE");

        if (loadFontToMemory(currentFontIndex)) {
            isFirstRender = true;
            uint32_t cp = findRandomGlyph();
            Serial.printf("Timer wake glyph: U+%04X\n", cp);
            if (currentViewMode == VIEW_OUTLINE)
                drawGlyphOutline(cp);
            else
                drawGlyph(cp);
            M5.Display.waitDisplay();
            Serial.println("Timer wake: display done");
            logBatteryDebug(cp, "timer");
        } else {
            Serial.println("ERROR: loadFontToMemory FAILED on timer wake!");
        }
        goToSleepAnchored(); // stay on schedule, no drift

    } else if (wakeReason == 2) {
        // ---- MANUAL WAKE (button): restore state + interactive mode ----
        // Display still shows last glyph (clear_display=false), just restore state
        Serial.println("Manual wake — restoring state, entering interactive mode");
        logBatteryBlankLine(); // separate from previous timer interval

        // Sanitize debug timer values
        if (config.wakeIntervalMinutes < 5) config.wakeIntervalMinutes = 15;

        // Restore last font/mode from NVS so interactive mode continues from where we were
        {
            Preferences prefs;
            prefs.begin("ps3", true);
            currentFontIndex = prefs.getInt("last_font", 0);
            currentViewMode = (ViewMode)prefs.getInt("last_mode", VIEW_BITMAP);
            prefs.end();

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
        loadFontToMemory(currentFontIndex);
        // Don't render a new glyph — display still shows the last one
        // Fall through to loop() for interactive touch + inactivity sleep

    } else {
        // ---- FIRST BOOT: splash + setup + first glyph ----
        Serial.println("First boot — setup screen");
        runSplashAndSetup();
        // Fall through to loop() for interactive mode + inactivity sleep
    }

    Serial.println("Interactive mode. Inactivity timeout: 60s. Hold center 10s: setup.");
}

// Inactivity timer for sleep
static unsigned long lastInteraction = 0;
#define INACTIVITY_TIMEOUT_MS 60000
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

    // Hold detection: center zone (180-360, 90-870) held 5s+ then released → full restart
    uint32_t holdDuration = millis() - touch.base_msec;
    if (touch.wasReleased() && holdDuration >= HOLD_FOR_SETUP_MS) {
        int x = touch.x;
        int y = touch.y;
        if (x >= 180 && x < 360 && y >= 90 && y < displayH - 90) {
            // Re-enable serial for setup (may have been disabled in normal mode)
            Serial.begin(115200);
            delay(500);
            Serial.printf("Long hold released (%dms) — full restart\n", holdDuration);
            M5.Display.waitDisplay();
            runSplashAndSetup();
            lastInteraction = millis();
            return;
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
            // Top or bottom strip: toggle view mode, keep same glyph and font
            currentViewMode = (currentViewMode == VIEW_BITMAP) ? VIEW_OUTLINE : VIEW_BITMAP;
            Serial.printf("Toggle mode: %s\n", currentViewMode == VIEW_BITMAP ? "BITMAP" : "OUTLINE");
            if (currentViewMode == VIEW_OUTLINE)
                drawGlyphOutline(currentCodepoint);
            else
                drawGlyph(currentCodepoint);
        }
        else if (x < TOUCH_COL_W) {
            // Left (x < 180): previous enabled font, keep same glyph and mode
            int start = currentFontIndex;
            do {
                currentFontIndex--;
                if (currentFontIndex < 0) currentFontIndex = fontCount - 1;
            } while (!config.fontEnabled[currentFontIndex] && currentFontIndex != start);
            Serial.printf("Prev font: %s\n", fontNames[currentFontIndex].c_str());
            if (loadFontToMemory(currentFontIndex)) {
                if (currentViewMode == VIEW_OUTLINE)
                    drawGlyphOutline(currentCodepoint);
                else
                    drawGlyph(currentCodepoint);
            }
        } else if (x >= TOUCH_COL_W * 2) {
            // Right (x >= 360): next enabled font, keep same glyph and mode
            int start = currentFontIndex;
            do {
                currentFontIndex++;
                if (currentFontIndex >= fontCount) currentFontIndex = 0;
            } while (!config.fontEnabled[currentFontIndex] && currentFontIndex != start);
            Serial.printf("Next font: %s\n", fontNames[currentFontIndex].c_str());
            if (loadFontToMemory(currentFontIndex)) {
                if (currentViewMode == VIEW_OUTLINE)
                    drawGlyphOutline(currentCodepoint);
                else
                    drawGlyph(currentCodepoint);
            }
        } else {
            // Center: random glyph, keep same font and mode
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
            Serial.println("Timed full refresh");
            partialRefreshCount = MAX_PARTIAL_BEFORE_FULL;
            if (currentViewMode == VIEW_OUTLINE)
                drawGlyphOutline(currentCodepoint);
            else
                drawGlyph(currentCodepoint);
        }
    }

    delay(20); // ~50Hz touch polling — saves CPU cycles vs 100Hz, still responsive
}
