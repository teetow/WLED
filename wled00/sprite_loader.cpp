#include "wled.h"

#ifdef ARDUINO_ARCH_ESP32

#include <PNGdec.h>

#include "prng.h"

namespace {

constexpr uint16_t SPRITE_MAX_DIMENSION = 320;
constexpr uint16_t SPRITE_MAX_INSTANCES = 64;
constexpr size_t SPRITE_PIXEL_BYTES = 3;
constexpr size_t SPRITE_CACHE_BUDGET = (MAX_SEGMENT_DATA > (12 * 1024)) ? (12 * 1024) : MAX_SEGMENT_DATA;
constexpr byte SPRITE_ERROR_NONE = 0;
constexpr byte SPRITE_ERROR_NO_NAME = 1;
constexpr byte SPRITE_ERROR_UNSUPPORTED = 2;
constexpr byte SPRITE_ERROR_FILE_MISSING = 3;
constexpr byte SPRITE_ERROR_BAD_ANIMDEF = 4;
constexpr byte SPRITE_ERROR_ALLOC = 5;
constexpr byte SPRITE_ERROR_PNG = 6;
constexpr byte SPRITE_ERROR_TOO_LARGE = 7;

struct SpriteFrameSpec {
  uint16_t durationMs;
  uint16_t width;
  uint16_t height;
  char filename[WLED_MAX_SEGNAME_LEN + 2];
};

struct SpriteFrame {
  uint16_t durationMs;
  uint16_t width;
  uint16_t height;
  uint32_t dataOffset;
};

struct SpritePixel {
  uint16_t color565;
  uint8_t alpha;
} __attribute__((packed));

struct SpriteStats {
  uint32_t loads;
  uint32_t loadErrors;
  uint32_t lastLoadMs;
  uint32_t peakCacheBytes;
  uint32_t draws;
  uint32_t skips;
  uint32_t blitPixels;
  uint32_t lastDrawUs;
  uint32_t maxDrawUs;
  uint16_t lastWidth;
  uint16_t lastHeight;
  uint16_t lastFrames;
  uint8_t lastError;
};

struct SpriteInstance {
  int16_t x;
  int16_t y;
};

struct SpriteState {
  char assetName[WLED_MAX_SEGNAME_LEN + 2];
  uint16_t frameCount;
  uint16_t currentFrame;
  uint32_t nextFrameTime;
  uint32_t pixelDataOffset;
  uint32_t cacheBytes;
  uint32_t layoutKey;
  uint32_t lastRenderKey;
  uint16_t maxWidth;
  uint16_t maxHeight;
  uint16_t instanceCount;
  uint8_t lastError;
};

struct SpriteRenderContext {
  Segment *segment;
  uint16_t segmentWidth;
  uint16_t segmentHeight;
  uint16_t spriteWidth;
  uint16_t spriteHeight;
  uint16_t motionOffset;
  uint16_t instanceCount;
  bool useMask;
  bool useGradient;
  bool useHorizontalGradient;
  bool moving;
  const SpriteInstance *instances;
};

struct SpriteCacheLoadContext {
  SpritePixel *pixels;
  uint16_t width;
  uint16_t height;
};

static File spritePngFile;
static SpriteStats spriteStats = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

void *spritePngOpen(const char *filename, int32_t *size);
void spritePngClose(void *handle);
int32_t spritePngRead(PNGFILE *handle, uint8_t *buffer, int32_t length);
int32_t spritePngSeek(PNGFILE *handle, int32_t position);
int spriteCacheWrite(PNGDRAW *pDraw);

inline SpriteFrame *getSpriteFrames(SpriteState *state) {
  return reinterpret_cast<SpriteFrame *>(state + 1);
}

inline SpriteInstance *getSpriteInstances(SpriteState *state) {
  return reinterpret_cast<SpriteInstance *>(getSpriteFrames(state) + state->frameCount);
}

inline uint8_t *getSpritePixelBlob(SpriteState *state) {
  return reinterpret_cast<uint8_t *>(state) + state->pixelDataOffset;
}

bool endsWithIgnoreCase(const char *value, const char *suffix) {
  if (!value || !suffix) return false;
  const size_t valueLen = strlen(value);
  const size_t suffixLen = strlen(suffix);
  if (suffixLen > valueLen) return false;
  value += valueLen - suffixLen;
  for (size_t index = 0; index < suffixLen; index++) {
    if (tolower(value[index]) != tolower(suffix[index])) return false;
  }
  return true;
}

void normalizeRootPath(const char *name, char (&buffer)[WLED_MAX_SEGNAME_LEN + 2]) {
  buffer[0] = '/';
  strncpy(buffer + 1, name, WLED_MAX_SEGNAME_LEN);
  buffer[WLED_MAX_SEGNAME_LEN + 1] = '\0';
}

void resolveFramePath(const char *assetPath, const char *filename, char (&buffer)[WLED_MAX_SEGNAME_LEN + 2]) {
  if (!filename || filename[0] == '\0') {
    buffer[0] = '\0';
    return;
  }

  if (filename[0] == '/') {
    strncpy(buffer, filename, WLED_MAX_SEGNAME_LEN + 1);
    buffer[WLED_MAX_SEGNAME_LEN + 1] = '\0';
    return;
  }

  const char *lastSlash = strrchr(assetPath, '/');
  size_t prefixLen = (lastSlash && lastSlash != assetPath) ? size_t(lastSlash - assetPath + 1) : 1U;
  if (prefixLen > WLED_MAX_SEGNAME_LEN + 1) prefixLen = WLED_MAX_SEGNAME_LEN + 1;
  memcpy(buffer, assetPath, prefixLen);
  buffer[prefixLen] = '\0';
  strncat(buffer, filename, WLED_MAX_SEGNAME_LEN + 1 - prefixLen);
}

uint32_t hashString32(const char *value) {
  uint32_t hash = 2166136261UL;
  while (value && *value) {
    hash ^= uint8_t(*value++);
    hash *= 16777619UL;
  }
  return hash;
}

uint8_t unpackPackedPixel(const uint8_t *source, uint16_t pixelIndex, uint8_t bitsPerPixel) {
  if (bitsPerPixel >= 8) return source[pixelIndex];
  const uint8_t mask = (1U << bitsPerPixel) - 1U;
  const uint8_t pixelsPerByte = 8U / bitsPerPixel;
  const uint8_t byteValue = source[pixelIndex / pixelsPerByte];
  const uint8_t shift = 8U - bitsPerPixel * ((pixelIndex % pixelsPerByte) + 1U);
  return (byteValue >> shift) & mask;
}

uint32_t rgbToRgb32(uint8_t red, uint8_t green, uint8_t blue) {
  return RGBW32(red, green, blue, 0);
}

uint16_t rgb32To565(uint32_t color) {
  return uint16_t(((R(color) & 0xF8) << 8) | ((G(color) & 0xFC) << 3) | (B(color) >> 3));
}

uint32_t rgb565ToRgb32(uint16_t color) {
  const uint8_t red = ((color >> 11) & 0x1F) * 255 / 31;
  const uint8_t green = ((color >> 5) & 0x3F) * 255 / 63;
  const uint8_t blue = (color & 0x1F) * 255 / 31;
  return rgbToRgb32(red, green, blue);
}

bool decodeSpritePixel(const PNGDRAW *pDraw, uint16_t pixelX, uint32_t &color, uint8_t &alpha) {
  const uint8_t *pixels = pDraw->pPixels;
  switch (pDraw->iPixelType) {
    case PNG_PIXEL_TRUECOLOR: {
      const uint8_t *pixel = pixels + pixelX * 3U;
      color = rgbToRgb32(pixel[0], pixel[1], pixel[2]);
      alpha = 255;
      return true;
    }
    case PNG_PIXEL_TRUECOLOR_ALPHA: {
      const uint8_t *pixel = pixels + pixelX * 4U;
      color = rgbToRgb32(pixel[0], pixel[1], pixel[2]);
      alpha = pixel[3];
      return true;
    }
    case PNG_PIXEL_GRAYSCALE: {
      const uint8_t value = unpackPackedPixel(pixels, pixelX, pDraw->iBpp);
      const uint16_t levels = (1U << pDraw->iBpp) - 1U;
      const uint8_t gray = levels ? uint8_t((uint16_t(value) * 255U) / levels) : 0;
      color = rgbToRgb32(gray, gray, gray);
      alpha = 255;
      return true;
    }
    case PNG_PIXEL_GRAY_ALPHA: {
      const uint8_t *pixel = pixels + pixelX * 2U;
      color = rgbToRgb32(pixel[0], pixel[0], pixel[0]);
      alpha = pixel[1];
      return true;
    }
    case PNG_PIXEL_INDEXED: {
      const uint8_t index = unpackPackedPixel(pixels, pixelX, pDraw->iBpp);
      const uint8_t *palette = &pDraw->pPalette[index * 3U];
      color = rgbToRgb32(palette[0], palette[1], palette[2]);
      alpha = pDraw->iHasAlpha ? pDraw->pPalette[768 + index] : 255;
      return true;
    }
    default:
      color = 0;
      alpha = 0;
      return false;
  }
}

void noteSpriteLoad(uint16_t width, uint16_t height, uint16_t frames, uint32_t cacheBytes, uint32_t loadMs) {
  spriteStats.loads++;
  spriteStats.lastLoadMs = loadMs;
  spriteStats.lastWidth = width;
  spriteStats.lastHeight = height;
  spriteStats.lastFrames = frames;
  if (cacheBytes > spriteStats.peakCacheBytes) spriteStats.peakCacheBytes = cacheBytes;
  spriteStats.lastError = SPRITE_ERROR_NONE;
}

void noteSpriteError(byte error) {
  if (error != SPRITE_ERROR_NONE) spriteStats.loadErrors++;
  spriteStats.lastError = error;
}

void noteSpriteDraw(uint32_t drawUs, uint32_t blitPixels, bool skipped) {
  if (skipped) {
    spriteStats.skips++;
    return;
  }

  spriteStats.draws++;
  spriteStats.blitPixels += blitPixels;
  spriteStats.lastDrawUs = drawUs;
  if (drawUs > spriteStats.maxDrawUs) spriteStats.maxDrawUs = drawUs;
}

size_t getActiveSpriteBytes() {
  size_t activeBytes = 0;
  for (size_t index = 0; index < strip.getSegmentsNum(); index++) {
    const Segment &segment = strip.getSegment(index);
    if (segment.isActive() && segment.mode == FX_MODE_SPRITE) activeBytes += segment.dataSize();
  }
  return activeBytes;
}

uint8_t getActiveSpriteSegments() {
  uint8_t activeSegments = 0;
  for (size_t index = 0; index < strip.getSegmentsNum(); index++) {
    const Segment &segment = strip.getSegment(index);
    if (segment.isActive() && segment.mode == FX_MODE_SPRITE) activeSegments++;
  }
  return activeSegments;
}

uint16_t getMaxInstanceCount(uint16_t segmentWidth, uint16_t segmentHeight, uint16_t spriteWidth, uint16_t spriteHeight) {
  if (segmentWidth == 0 || segmentHeight == 0 || spriteWidth == 0 || spriteHeight == 0) return 1;
  if (spriteWidth >= segmentWidth || spriteHeight >= segmentHeight) return 1;

  const uint32_t spriteArea = uint32_t(spriteWidth) * spriteHeight;
  const uint32_t segmentArea = uint32_t(segmentWidth) * segmentHeight;
  const uint16_t fitLimit = min<uint16_t>(SPRITE_MAX_INSTANCES, max<uint32_t>(1, segmentArea / max<uint32_t>(1, spriteArea)));
  return max<uint16_t>(1, fitLimit);
}

uint16_t getDesiredInstanceCount(const Segment &seg, uint16_t segmentWidth, uint16_t segmentHeight, uint16_t spriteWidth, uint16_t spriteHeight) {
  const uint16_t maxInstances = getMaxInstanceCount(segmentWidth, segmentHeight, spriteWidth, spriteHeight);
  if (maxInstances <= 1) return 1;
  return 1 + map(seg.intensity, 0, 255, 0, maxInstances - 1);
}

void generateInstanceLayout(SpriteState &state, uint16_t requestedCount, uint16_t segmentWidth, uint16_t segmentHeight, uint16_t spriteWidth, uint16_t spriteHeight, uint32_t layoutKey) {
  SpriteInstance *instances = getSpriteInstances(&state);
  const int16_t centeredX = (int16_t(segmentWidth) - int16_t(spriteWidth)) / 2;
  const int16_t centeredY = (int16_t(segmentHeight) - int16_t(spriteHeight)) / 2;

  state.layoutKey = layoutKey;
  state.instanceCount = 1;
  instances[0] = { centeredX, centeredY };

  if (requestedCount <= 1 || spriteWidth >= segmentWidth || spriteHeight >= segmentHeight) return;

  const int16_t maxX = max<int16_t>(0, int16_t(segmentWidth) - int16_t(spriteWidth));
  const int16_t maxY = max<int16_t>(0, int16_t(segmentHeight) - int16_t(spriteHeight));
  if (maxX == 0 || maxY == 0) return;

  const uint32_t availableArea = uint32_t(maxX + 1) * uint32_t(maxY + 1);
  const uint16_t spriteSpan = max<uint16_t>(1, max(spriteWidth, spriteHeight));
  uint16_t minDistance = max<uint16_t>(spriteSpan, uint16_t(sqrtf(float(availableArea) / float(requestedCount)) * 0.65f));
  uint32_t minDistanceSq = uint32_t(minDistance) * minDistance;
  const int16_t spriteHalfWidth = spriteWidth / 2;
  const int16_t spriteHalfHeight = spriteHeight / 2;
  PRNG prng(uint16_t(layoutKey ^ (layoutKey >> 16)));

  uint16_t attempts = 0;
  const uint16_t maxAttempts = requestedCount * 240;
  const uint16_t relaxEvery = max<uint16_t>(16, requestedCount * 12);
  while (state.instanceCount < requestedCount && attempts < maxAttempts) {
    const int16_t candidateX = prng.random16(maxX + 1);
    const int16_t candidateY = prng.random16(maxY + 1);
    const int16_t candidateCenterX = candidateX + spriteHalfWidth;
    const int16_t candidateCenterY = candidateY + spriteHalfHeight;
    bool valid = true;

    for (uint16_t index = 0; index < state.instanceCount; index++) {
      const int32_t dx = candidateCenterX - (instances[index].x + spriteHalfWidth);
      const int32_t dy = candidateCenterY - (instances[index].y + spriteHalfHeight);
      if (uint32_t(dx * dx + dy * dy) < minDistanceSq) {
        valid = false;
        break;
      }
    }

    if (valid) {
      instances[state.instanceCount++] = { candidateX, candidateY };
      continue;
    }

    attempts++;
    if (attempts % relaxEvery == 0 && minDistance > 1) {
      minDistance = max<uint16_t>(1, uint16_t((uint32_t(minDistance) * 7U) / 8U));
      minDistanceSq = uint32_t(minDistance) * minDistance;
    }
  }

  while (state.instanceCount < requestedCount) {
    instances[state.instanceCount++] = { int16_t(prng.random16(maxX + 1)), int16_t(prng.random16(maxY + 1)) };
  }
}

void ensureInstanceLayout(SpriteState &state, const Segment &seg, uint16_t spriteWidth, uint16_t spriteHeight) {
  const uint16_t segmentWidth = seg.vWidth();
  const uint16_t segmentHeight = seg.vHeight();
  const uint16_t desiredCount = getDesiredInstanceCount(seg, segmentWidth, segmentHeight, spriteWidth, spriteHeight);
  uint32_t layoutKey = hashString32(state.assetName);
  layoutKey ^= uint32_t(segmentWidth) << 20;
  layoutKey ^= uint32_t(segmentHeight) << 12;
  layoutKey ^= uint32_t(spriteWidth) << 6;
  layoutKey ^= uint32_t(spriteHeight);
  layoutKey ^= uint32_t(desiredCount) << 26;

  if (state.layoutKey == layoutKey && state.instanceCount == desiredCount) return;
  generateInstanceLayout(state, desiredCount, segmentWidth, segmentHeight, spriteWidth, spriteHeight, layoutKey);
}

bool allocateSpriteState(Segment &seg, const char *assetName, const SpriteFrameSpec *specs, uint16_t frameCount, uint32_t pixelBytes, SpriteState *&state) {
  const size_t metadataBytes = sizeof(SpriteState) + size_t(frameCount) * sizeof(SpriteFrame) + size_t(SPRITE_MAX_INSTANCES) * sizeof(SpriteInstance);
  const size_t dataSize = metadataBytes + pixelBytes;
  seg.deallocateData();
  if (!seg.allocateData(dataSize)) return false;

  state = reinterpret_cast<SpriteState *>(seg.data);
  strlcpy(state->assetName, assetName, sizeof(state->assetName));
  state->frameCount = frameCount;
  state->currentFrame = 0;
  state->nextFrameTime = 0;
  state->pixelDataOffset = metadataBytes;
  state->cacheBytes = pixelBytes;
  state->layoutKey = 0;
  state->lastRenderKey = UINT32_MAX;
  state->maxWidth = 0;
  state->maxHeight = 0;
  state->instanceCount = 0;
  state->lastError = SPRITE_ERROR_NONE;

  SpriteFrame *frames = getSpriteFrames(state);
  uint32_t dataOffset = 0;
  for (uint16_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
    frames[frameIndex].durationMs = specs[frameIndex].durationMs;
    frames[frameIndex].width = specs[frameIndex].width;
    frames[frameIndex].height = specs[frameIndex].height;
    frames[frameIndex].dataOffset = dataOffset;
    dataOffset += uint32_t(specs[frameIndex].width) * specs[frameIndex].height * sizeof(SpritePixel);
    if (specs[frameIndex].width > state->maxWidth) state->maxWidth = specs[frameIndex].width;
    if (specs[frameIndex].height > state->maxHeight) state->maxHeight = specs[frameIndex].height;
  }

  return true;
}

bool inspectSpriteSpecs(SpriteFrameSpec *specs, uint16_t frameCount, uint16_t &maxWidth, uint16_t &maxHeight, uint32_t &pixelBytes) {
  PNG *decoder = new PNG();
  if (!decoder) return false;

  pixelBytes = 0;
  maxWidth = 0;
  maxHeight = 0;
  bool success = true;
  for (uint16_t frameIndex = 0; frameIndex < frameCount; frameIndex++) {
    const int openResult = decoder->open(specs[frameIndex].filename, spritePngOpen, spritePngClose, spritePngRead, spritePngSeek, nullptr);
    if (openResult != PNG_SUCCESS) {
      success = false;
      break;
    }

    specs[frameIndex].width = decoder->getWidth();
    specs[frameIndex].height = decoder->getHeight();
    decoder->close();
    if (specs[frameIndex].width == 0 || specs[frameIndex].height == 0 || specs[frameIndex].width > SPRITE_MAX_DIMENSION || specs[frameIndex].height > SPRITE_MAX_DIMENSION) {
      success = false;
      break;
    }

    if (specs[frameIndex].width > maxWidth) maxWidth = specs[frameIndex].width;
    if (specs[frameIndex].height > maxHeight) maxHeight = specs[frameIndex].height;
    pixelBytes += uint32_t(specs[frameIndex].width) * specs[frameIndex].height * sizeof(SpritePixel);
    if (pixelBytes > SPRITE_CACHE_BUDGET) {
      success = false;
      break;
    }
  }

  delete decoder;
  return success;
}

bool decodeSpriteCache(SpriteState &state, const SpriteFrameSpec *specs) {
  PNG *decoder = new PNG();
  if (!decoder) return false;

  bool success = true;
  SpriteFrame *frames = getSpriteFrames(&state);
  for (uint16_t frameIndex = 0; frameIndex < state.frameCount; frameIndex++) {
    const int openResult = decoder->open(specs[frameIndex].filename, spritePngOpen, spritePngClose, spritePngRead, spritePngSeek, spriteCacheWrite);
    if (openResult != PNG_SUCCESS) {
      success = false;
      break;
    }

    SpriteCacheLoadContext context = {
      .pixels = reinterpret_cast<SpritePixel *>(getSpritePixelBlob(&state) + frames[frameIndex].dataOffset),
      .width = frames[frameIndex].width,
      .height = frames[frameIndex].height,
    };

    const int decodeResult = decoder->decode(&context, PNG_FAST_PALETTE);
    decoder->close();
    if (decodeResult != PNG_SUCCESS) {
      success = false;
      break;
    }
  }

  delete decoder;
  return success;
}

bool loadSpriteFromSpecs(Segment &seg, const char *assetName, SpriteFrameSpec *specs, uint16_t frameCount, SpriteState *&state, byte &errorCode) {
  const uint32_t start = millis();
  uint16_t maxWidth = 0;
  uint16_t maxHeight = 0;
  uint32_t pixelBytes = 0;
  if (!inspectSpriteSpecs(specs, frameCount, maxWidth, maxHeight, pixelBytes)) {
    errorCode = (pixelBytes > SPRITE_CACHE_BUDGET) ? SPRITE_ERROR_TOO_LARGE : SPRITE_ERROR_PNG;
    return false;
  }

  if (!allocateSpriteState(seg, assetName, specs, frameCount, pixelBytes, state)) {
    errorCode = SPRITE_ERROR_ALLOC;
    return false;
  }

  if (!decodeSpriteCache(*state, specs)) {
    seg.deallocateData();
    state = nullptr;
    errorCode = SPRITE_ERROR_PNG;
    return false;
  }

  noteSpriteLoad(maxWidth, maxHeight, frameCount, pixelBytes, millis() - start);
  errorCode = SPRITE_ERROR_NONE;
  return true;
}

byte loadSingleSprite(Segment &seg, const char *assetName, SpriteState *&state) {
  auto *specs = static_cast<SpriteFrameSpec *>(calloc(1, sizeof(SpriteFrameSpec)));
  if (!specs) return SPRITE_ERROR_ALLOC;
  specs[0].durationMs = 0;
  strlcpy(specs[0].filename, assetName, sizeof(specs[0].filename));

  byte errorCode = SPRITE_ERROR_NONE;
  const bool success = loadSpriteFromSpecs(seg, assetName, specs, 1, state, errorCode);
  free(specs);
  return success ? SPRITE_ERROR_NONE : errorCode;
}

byte loadAnimDef(Segment &seg, const char *assetName, SpriteState *&state) {
  File file = WLED_FS.open(assetName, "r");
  if (!file) return SPRITE_ERROR_FILE_MISSING;

  const size_t capacity = min<size_t>(JSON_BUFFER_SIZE, max<size_t>(512, file.size() * 4));
  PSRAMDynamicJsonDocument doc(capacity);
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error || !doc.is<JsonArray>()) return SPRITE_ERROR_BAD_ANIMDEF;

  JsonArray framesJson = doc.as<JsonArray>();
  if (framesJson.isNull() || framesJson.size() == 0) return SPRITE_ERROR_BAD_ANIMDEF;

  auto *specs = static_cast<SpriteFrameSpec *>(calloc(framesJson.size(), sizeof(SpriteFrameSpec)));
  if (!specs) return SPRITE_ERROR_ALLOC;

  uint16_t frameIndex = 0;
  byte errorCode = SPRITE_ERROR_BAD_ANIMDEF;
  for (JsonObject frameJson : framesJson) {
    const char *filename = frameJson["filename"] | "";
    if (filename[0] == '\0') {
      free(specs);
      return SPRITE_ERROR_BAD_ANIMDEF;
    }
    resolveFramePath(assetName, filename, specs[frameIndex].filename);
    if (specs[frameIndex].filename[0] == '\0') {
      free(specs);
      return SPRITE_ERROR_BAD_ANIMDEF;
    }
    specs[frameIndex].durationMs = max<uint16_t>(1, frameJson["duration"] | 100);
    frameIndex++;
  }

  const bool success = loadSpriteFromSpecs(seg, assetName, specs, frameIndex, state, errorCode);
  free(specs);
  return success ? SPRITE_ERROR_NONE : errorCode;
}

byte ensureSpriteState(Segment &seg, SpriteState *&state) {
  if (!seg.name) return SPRITE_ERROR_NO_NAME;

  char assetName[WLED_MAX_SEGNAME_LEN + 2];
  normalizeRootPath(seg.name, assetName);

  state = reinterpret_cast<SpriteState *>(seg.data);
  if (state && strncmp(state->assetName, assetName, sizeof(state->assetName)) == 0) return SPRITE_ERROR_NONE;

  byte errorCode = SPRITE_ERROR_UNSUPPORTED;
  if (endsWithIgnoreCase(assetName, ".png")) {
    errorCode = loadSingleSprite(seg, assetName, state);
  } else if (endsWithIgnoreCase(assetName, ".json")) {
    errorCode = loadAnimDef(seg, assetName, state);
  }

  if (state) state->lastError = errorCode;
  noteSpriteError(errorCode);
  return errorCode;
}

uint16_t currentFrameDuration(const SpriteState *state) {
  if (!state || state->frameCount == 0) return 0;
  const SpriteFrame *frames = getSpriteFrames(const_cast<SpriteState *>(state));
  return frames[state->currentFrame].durationMs;
}

inline uint32_t rotl32(uint32_t value, uint8_t shift) {
  return (value << shift) | (value >> (32 - shift));
}

uint32_t buildRenderKey(const Segment &seg, const SpriteState &state, uint16_t motionOffset) {
  uint32_t key = state.layoutKey;
  key ^= uint32_t(state.currentFrame) << 24;
  key ^= uint32_t(motionOffset) << 8;
  key ^= uint32_t(seg.check1) << 2;
  key ^= uint32_t(seg.check2) << 3;
  key ^= uint32_t(seg.check3) << 4;
  key ^= uint32_t(seg.palette) << 16;
  key ^= rotl32(seg.colors[0], 5);
  key ^= rotl32(seg.colors[1], 13);
  key ^= rotl32(seg.colors[2], 21);
  return key;
}

void updateAnimationFrame(SpriteState *state) {
  if (!state || state->frameCount <= 1) return;

  if (state->nextFrameTime == 0) {
    state->nextFrameTime = strip.now + currentFrameDuration(state);
    return;
  }

  while ((int32_t)(strip.now - state->nextFrameTime) >= 0) {
    state->currentFrame = (state->currentFrame + 1) % state->frameCount;
    state->nextFrameTime += currentFrameDuration(state);
  }
}

uint32_t getMaskColor(const SpriteRenderContext &context, int16_t destX, int16_t destY) {
  if (!context.useGradient) return SEGCOLOR(0);

  uint8_t paletteIndex = 0;
  if (context.useHorizontalGradient) {
    if (context.segmentWidth <= 1) return SEGCOLOR(0);
    paletteIndex = map(destX, 0, context.segmentWidth - 1, 0, 255);
  } else {
    if (context.segmentHeight <= 1) return SEGCOLOR(0);
    paletteIndex = map(destY, 0, context.segmentHeight - 1, 0, 255);
  }

  return context.segment->color_from_palette(paletteIndex, false, false, 0);
}

void blendSpritePixel(const SpriteRenderContext &context, int16_t destX, int16_t destY, uint32_t sourceColor, uint8_t alpha) {
  if (alpha == 0) return;
  if ((unsigned)destX >= context.segmentWidth || (unsigned)destY >= context.segmentHeight) return;

  const uint32_t outputColor = context.useMask ? getMaskColor(context, destX, destY) : sourceColor;
  context.segment->blendPixelColorXY(destX, destY, outputColor, alpha);
}

int spriteCacheWrite(PNGDRAW *pDraw) {
  auto *context = static_cast<SpriteCacheLoadContext *>(pDraw->pUser);
  if (!context) return 0;
  if (pDraw->y < 0 || pDraw->y >= context->height) return 0;
  if (pDraw->iWidth <= 0 || pDraw->iWidth > context->width) return 0;

  SpritePixel *row = context->pixels + size_t(pDraw->y) * context->width;
  for (uint16_t pixelX = 0; pixelX < context->width; pixelX++) {
    row[pixelX].color565 = 0;
    row[pixelX].alpha = 0;
  }

  for (int pixelX = 0; pixelX < pDraw->iWidth; pixelX++) {
    uint32_t color;
    uint8_t alpha;
    if (!decodeSpritePixel(pDraw, pixelX, color, alpha) || alpha == 0) continue;
    row[pixelX].color565 = rgb32To565(color);
    row[pixelX].alpha = alpha;
  }

  return 1;
}

void *spritePngOpen(const char *filename, int32_t *size) {
  spritePngFile = WLED_FS.open(filename, FILE_READ);
  if (!spritePngFile) return nullptr;
  *size = spritePngFile.size();
  return &spritePngFile;
}

void spritePngClose(void *handle) {
  auto *pngFile = static_cast<File *>(handle);
  if (pngFile) pngFile->close();
}

int32_t spritePngRead(PNGFILE *handle, uint8_t *buffer, int32_t length) {
  auto *pngFile = static_cast<File *>(handle->fHandle);
  return pngFile ? int32_t(pngFile->read(buffer, length)) : 0;
}

int32_t spritePngSeek(PNGFILE *handle, int32_t position) {
  auto *pngFile = static_cast<File *>(handle->fHandle);
  return (pngFile && pngFile->seek(position)) ? 1 : 0;
}

uint16_t computeMotionOffset(uint8_t speed, uint16_t segmentWidth, uint16_t spriteWidth) {
  if (speed == 0 || segmentWidth == 0 || spriteWidth > segmentWidth) return 0;
  const uint16_t pixelsPerSecond = map(speed, 1, 255, 4, 128);
  return ((strip.now * pixelsPerSecond) / 1000U) % segmentWidth;
}

byte drawSpriteFrame(Segment &seg, SpriteState &state, const SpriteFrame &frame) {
  ensureInstanceLayout(state, seg, state.maxWidth, state.maxHeight);
  const uint16_t motionOffset = computeMotionOffset(seg.speed, seg.vWidth(), state.maxWidth);
  const uint32_t renderKey = buildRenderKey(seg, state, motionOffset);
  if (state.lastRenderKey == renderKey) {
    noteSpriteDraw(0, 0, true);
    return SPRITE_ERROR_NONE;
  }

  SpriteRenderContext context = {
    .segment = &seg,
    .segmentWidth = uint16_t(seg.vWidth()),
    .segmentHeight = uint16_t(seg.vHeight()),
    .spriteWidth = frame.width,
    .spriteHeight = frame.height,
    .motionOffset = motionOffset,
    .instanceCount = state.instanceCount,
    .useMask = seg.check1,
    .useGradient = seg.check1 && seg.check2,
    .useHorizontalGradient = seg.check3,
    .moving = seg.speed > 0 && state.maxWidth <= seg.vWidth(),
    .instances = getSpriteInstances(&state),
  };

  const uint32_t drawStart = micros();
  uint32_t blitPixels = 0;
  seg.fill(BLACK);
  const auto *pixels = reinterpret_cast<const SpritePixel *>(getSpritePixelBlob(&state) + frame.dataOffset);
  for (uint16_t y = 0; y < frame.height; y++) {
    const SpritePixel *row = pixels + size_t(y) * frame.width;
    for (uint16_t x = 0; x < frame.width; x++) {
      if (row[x].alpha == 0) continue;
      const uint32_t color = context.useMask ? SEGCOLOR(0) : rgb565ToRgb32(row[x].color565);
      for (uint16_t instanceIndex = 0; instanceIndex < context.instanceCount; instanceIndex++) {
        int16_t drawX = context.instances[instanceIndex].x;
        const int16_t drawY = context.instances[instanceIndex].y;
        if (context.moving) {
          int32_t movedX = drawX + context.motionOffset;
          movedX %= int32_t(context.segmentWidth);
          if (movedX < 0) movedX += context.segmentWidth;
          drawX = movedX;
        }

        const int16_t destX = drawX + x;
        const int16_t destY = drawY + y;
        blendSpritePixel(context, destX, destY, color, row[x].alpha);
        blitPixels++;

        if (context.moving && context.spriteWidth <= context.segmentWidth && destX >= context.segmentWidth) {
          blendSpritePixel(context, destX - context.segmentWidth, destY, color, row[x].alpha);
          blitPixels++;
        }
      }
    }
  }

  state.lastRenderKey = renderKey;
  noteSpriteDraw(micros() - drawStart, blitPixels, false);
  return SPRITE_ERROR_NONE;
}

} // namespace

void endSpritePlayback(Segment *seg) {
  if (!seg || !seg->data) return;
  auto *state = reinterpret_cast<SpriteState *>(seg->data);
  state->lastRenderKey = UINT32_MAX;
}

void addSpriteInfo(JsonObject root) {
  JsonObject spriteInfo = root.createNestedObject(F("sprite"));
  spriteInfo[F("mode")] = F("cache");
  spriteInfo[F("budget")] = SPRITE_CACHE_BUDGET;
  spriteInfo[F("active")] = getActiveSpriteSegments();
  spriteInfo[F("bytes")] = getActiveSpriteBytes();
  spriteInfo[F("peak")] = spriteStats.peakCacheBytes;
  spriteInfo[F("loads")] = spriteStats.loads;
  spriteInfo[F("errors")] = spriteStats.loadErrors;
  spriteInfo[F("loadms")] = spriteStats.lastLoadMs;
  spriteInfo[F("frames")] = spriteStats.lastFrames;
  spriteInfo[F("w")] = spriteStats.lastWidth;
  spriteInfo[F("h")] = spriteStats.lastHeight;
  spriteInfo[F("draws")] = spriteStats.draws;
  spriteInfo[F("skips")] = spriteStats.skips;
  spriteInfo[F("pixels")] = spriteStats.blitPixels;
  spriteInfo[F("drawus")] = spriteStats.lastDrawUs;
  spriteInfo[F("drawmax")] = spriteStats.maxDrawUs;
  spriteInfo[F("err")] = spriteStats.lastError;
}

byte renderSpriteToSegment(Segment &seg) {
  if (!seg.name) return SPRITE_ERROR_NO_NAME;
  if (!strip.isMatrix || !seg.is2D()) return SPRITE_ERROR_UNSUPPORTED;

  SpriteState *state = reinterpret_cast<SpriteState *>(seg.data);
  const byte stateResult = ensureSpriteState(seg, state);
  if (stateResult != SPRITE_ERROR_NONE) return stateResult;
  if (!state || state->frameCount == 0) return SPRITE_ERROR_BAD_ANIMDEF;

  updateAnimationFrame(state);

  const SpriteFrame *frames = getSpriteFrames(state);
  return drawSpriteFrame(seg, *state, frames[state->currentFrame]);
}

#endif