#include "video-software.h"

#include "gba.h"
#include "gba-io.h"

#include <string.h>

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam);
static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y);
static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer);

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer);
static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPA(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPB(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPC(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPD(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGX_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGX_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGY_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGY_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value);

static void _drawScanline(struct GBAVideoSoftwareRenderer* renderer, int y);
static void _drawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y);
static void _drawBackgroundMode2(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y);
static void _drawBackgroundMode3(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y);
static void _drawBackgroundMode4(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y);
static void _drawBackgroundMode5(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y);
static void _preprocessTransformedSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBATransformedObj* sprite, int y);
static void _preprocessSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBAObj* sprite, int y);
static void _postprocessSprite(struct GBAVideoSoftwareRenderer* renderer, int priority);

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer);
static inline uint32_t _brighten(uint32_t color, int y);
static inline uint32_t _darken(uint32_t color, int y);
static uint32_t _mix(int weightA, uint32_t colorA, int weightB, uint32_t colorB);

void GBAVideoSoftwareRendererCreate(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->d.init = GBAVideoSoftwareRendererInit;
	renderer->d.deinit = GBAVideoSoftwareRendererDeinit;
	renderer->d.writeVideoRegister = GBAVideoSoftwareRendererWriteVideoRegister;
	renderer->d.writeOAM = GBAVideoSoftwareRendererWriteOAM;
	renderer->d.writePalette = GBAVideoSoftwareRendererWritePalette;
	renderer->d.drawScanline = GBAVideoSoftwareRendererDrawScanline;
	renderer->d.finishFrame = GBAVideoSoftwareRendererFinishFrame;

	renderer->d.turbo = 0;
	renderer->d.framesPending = 0;
	renderer->d.frameskip = 0;

	{
		pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
		renderer->mutex = mutex;
		pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
		renderer->upCond = cond;
		renderer->downCond = cond;
	}
}

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	int i;

	softwareRenderer->dispcnt.packed = 0x0080;

	softwareRenderer->target1Obj = 0;
	softwareRenderer->target1Bd = 0;
	softwareRenderer->target2Obj = 0;
	softwareRenderer->target2Bd = 0;
	softwareRenderer->blendEffect = BLEND_NONE;
	memset(softwareRenderer->normalPalette, 0, sizeof(softwareRenderer->normalPalette));
	memset(softwareRenderer->variantPalette, 0, sizeof(softwareRenderer->variantPalette));
	memset(softwareRenderer->enabledBitmap, 0, sizeof(softwareRenderer->enabledBitmap));

	softwareRenderer->blda = 0;
	softwareRenderer->bldb = 0;
	softwareRenderer->bldy = 0;

	for (i = 0; i < 4; ++i) {
		struct GBAVideoSoftwareBackground* bg = &softwareRenderer->bg[i];
		bg->index = i;
		bg->enabled = 0;
		bg->priority = 0;
		bg->charBase = 0;
		bg->mosaic = 0;
		bg->multipalette = 0;
		bg->screenBase = 0;
		bg->overflow = 0;
		bg->size = 0;
		bg->target1 = 0;
		bg->target2 = 0;
		bg->x = 0;
		bg->y = 0;
		bg->refx = 0;
		bg->refy = 0;
		bg->dx = 256;
		bg->dmx = 0;
		bg->dy = 0;
		bg->dmy = 256;
		bg->sx = 0;
		bg->sy = 0;
	}

	pthread_mutex_init(&softwareRenderer->mutex, 0);
	pthread_cond_init(&softwareRenderer->upCond, 0);
	pthread_cond_init(&softwareRenderer->downCond, 0);
}

static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	pthread_mutex_lock(&softwareRenderer->mutex);
	pthread_cond_broadcast(&softwareRenderer->upCond);
	pthread_mutex_unlock(&softwareRenderer->mutex);

	pthread_mutex_destroy(&softwareRenderer->mutex);
	pthread_cond_destroy(&softwareRenderer->upCond);
	pthread_cond_destroy(&softwareRenderer->downCond);
}

static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	switch (address) {
	case REG_DISPCNT:
		value &= 0xFFFB;
		softwareRenderer->dispcnt.packed = value;
		GBAVideoSoftwareRendererUpdateDISPCNT(softwareRenderer);
		break;
	case REG_BG0CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[0], value);
		break;
	case REG_BG1CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[1], value);
		break;
	case REG_BG2CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[2], value);
		break;
	case REG_BG3CNT:
		value &= 0xFFCF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[3], value);
		break;
	case REG_BG0HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].x = value;
		break;
	case REG_BG0VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].y = value;
		break;
	case REG_BG1HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].x = value;
		break;
	case REG_BG1VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].y = value;
		break;
	case REG_BG2HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].x = value;
		break;
	case REG_BG2VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].y = value;
		break;
	case REG_BG3HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].x = value;
		break;
	case REG_BG3VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].y = value;
		break;
	case REG_BG2PA:
		GBAVideoSoftwareRendererWriteBGPA(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2PB:
		GBAVideoSoftwareRendererWriteBGPB(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2PC:
		GBAVideoSoftwareRendererWriteBGPC(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2PD:
		GBAVideoSoftwareRendererWriteBGPD(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2X_LO:
		GBAVideoSoftwareRendererWriteBGX_LO(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2X_HI:
		GBAVideoSoftwareRendererWriteBGX_HI(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2Y_LO:
		GBAVideoSoftwareRendererWriteBGY_LO(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2Y_HI:
		GBAVideoSoftwareRendererWriteBGY_HI(&softwareRenderer->bg[2], value);
		break;
	case REG_BG3PA:
		GBAVideoSoftwareRendererWriteBGPA(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3PB:
		GBAVideoSoftwareRendererWriteBGPB(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3PC:
		GBAVideoSoftwareRendererWriteBGPC(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3PD:
		GBAVideoSoftwareRendererWriteBGPD(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3X_LO:
		GBAVideoSoftwareRendererWriteBGX_LO(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3X_HI:
		GBAVideoSoftwareRendererWriteBGX_HI(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3Y_LO:
		GBAVideoSoftwareRendererWriteBGY_LO(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3Y_HI:
		GBAVideoSoftwareRendererWriteBGY_HI(&softwareRenderer->bg[3], value);
		break;
	case REG_BLDCNT:
		GBAVideoSoftwareRendererWriteBLDCNT(softwareRenderer, value);
		break;
	case REG_BLDALPHA:
		softwareRenderer->blda = value & 0x1F;
		if (softwareRenderer->blda > 0x10) {
			softwareRenderer->blda = 0x10;
		}
		softwareRenderer->bldb = (value >> 8) & 0x1F;
		if (softwareRenderer->bldb > 0x10) {
			softwareRenderer->bldb = 0x10;
		}
		break;
	case REG_BLDY:
		softwareRenderer->bldy = value & 0x1F;
		if (softwareRenderer->bldy > 0x10) {
			softwareRenderer->bldy = 0x10;
		}
		_updatePalettes(softwareRenderer);
		break;
	default:
		GBALog(GBA_LOG_STUB, "Stub video register write: %03x", address);
	}
	return value;
}

static void GBAVideoSoftwareRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	if ((oam & 0x3) != 0x3) {
		oam >>= 2;
		struct GBAObj* sprite = &renderer->oam->obj[oam];
		int enabled = sprite->transformed || !sprite->disable;
		enabled <<= (oam & 0x1F);
		softwareRenderer->enabledBitmap[oam >> 5] = (softwareRenderer->enabledBitmap[oam >> 5] & ~(1 << (oam & 0x1F))) | enabled;
	}
}

static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	uint32_t color32 = 0;
	color32 |= (value << 3) & 0xF8;
	color32 |= (value << 6) & 0xF800;
	color32 |= (value << 9) & 0xF80000;
	softwareRenderer->normalPalette[address >> 1] = color32;
	if (softwareRenderer->blendEffect == BLEND_BRIGHTEN) {
		softwareRenderer->variantPalette[address >> 1] = _brighten(color32, softwareRenderer->bldy);
	} else if (softwareRenderer->blendEffect == BLEND_DARKEN) {
		softwareRenderer->variantPalette[address >> 1] = _darken(color32, softwareRenderer->bldy);
	}
}

static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	if (renderer->frameskip > 0) {
		return;
	}
	uint32_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
	if (softwareRenderer->dispcnt.forcedBlank) {
		for (int x = 0; x < VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = GBA_COLOR_WHITE;
		}
		return;
	} else {
		uint32_t backdrop = FLAG_UNWRITTEN | FLAG_PRIORITY | FLAG_IS_BACKGROUND;
		if (!softwareRenderer->target1Bd || softwareRenderer->blendEffect == BLEND_NONE || softwareRenderer->blendEffect == BLEND_ALPHA) {
			backdrop |= softwareRenderer->normalPalette[0];
		} else {
			backdrop |= softwareRenderer->variantPalette[0];
		}
		for (int x = 0; x < VIDEO_HORIZONTAL_PIXELS; ++x) {
			row[x] = backdrop;
		}
	}

	softwareRenderer->row = row;

	softwareRenderer->start = 0;
	softwareRenderer->end = VIDEO_HORIZONTAL_PIXELS;
	_drawScanline(softwareRenderer, y);
}

static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	pthread_mutex_lock(&softwareRenderer->mutex);
	if (renderer->frameskip > 0) {
		--renderer->frameskip;
	} else {
		renderer->framesPending++;
		pthread_cond_broadcast(&softwareRenderer->upCond);
		if (!renderer->turbo) {
			pthread_cond_wait(&softwareRenderer->downCond, &softwareRenderer->mutex);
		}
	}
	pthread_mutex_unlock(&softwareRenderer->mutex);

	softwareRenderer->bg[2].sx = softwareRenderer->bg[2].refx;
	softwareRenderer->bg[2].sy = softwareRenderer->bg[2].refy;
	softwareRenderer->bg[3].sx = softwareRenderer->bg[3].refx;
	softwareRenderer->bg[3].sy = softwareRenderer->bg[3].refy;
}

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->bg[0].enabled = renderer->dispcnt.bg0Enable;
	renderer->bg[1].enabled = renderer->dispcnt.bg1Enable;
	renderer->bg[2].enabled = renderer->dispcnt.bg2Enable;
	renderer->bg[3].enabled = renderer->dispcnt.bg3Enable;
}

static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	(void)(renderer);
	union GBARegisterBGCNT reg = { .packed = value };
	bg->priority = reg.priority;
	bg->charBase = reg.charBase << 14;
	bg->mosaic = reg.mosaic;
	bg->multipalette = reg.multipalette;
	bg->screenBase = reg.screenBase << 11;
	bg->overflow = reg.overflow;
	bg->size = reg.size;
}

static void GBAVideoSoftwareRendererWriteBGPA(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dx = value;
}

static void GBAVideoSoftwareRendererWriteBGPB(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dmx = value;
}

static void GBAVideoSoftwareRendererWriteBGPC(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dy = value;
}

static void GBAVideoSoftwareRendererWriteBGPD(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dmy = value;
}

static void GBAVideoSoftwareRendererWriteBGX_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refx = (bg->refx & 0xFFFF0000) | value;
	bg->sx = bg->refx;
}

static void GBAVideoSoftwareRendererWriteBGX_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refx = (bg->refx & 0x0000FFFF) | (value << 16);
	bg->refx <<= 4;
	bg->refx >>= 4;
	bg->sx = bg->refx;
}

static void GBAVideoSoftwareRendererWriteBGY_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refy = (bg->refy & 0xFFFF0000) | value;
	bg->sy = bg->refy;
}

static void GBAVideoSoftwareRendererWriteBGY_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refy = (bg->refy & 0x0000FFFF) | (value << 16);
	bg->refy <<= 4;
	bg->refy >>= 4;
	bg->sy = bg->refy;
}

static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value) {
	union {
		struct {
			unsigned target1Bg0 : 1;
			unsigned target1Bg1 : 1;
			unsigned target1Bg2 : 1;
			unsigned target1Bg3 : 1;
			unsigned target1Obj : 1;
			unsigned target1Bd : 1;
			enum BlendEffect effect : 2;
			unsigned target2Bg0 : 1;
			unsigned target2Bg1 : 1;
			unsigned target2Bg2 : 1;
			unsigned target2Bg3 : 1;
			unsigned target2Obj : 1;
			unsigned target2Bd : 1;
		};
		uint16_t packed;
	} bldcnt = { .packed = value };

	enum BlendEffect oldEffect = renderer->blendEffect;

	renderer->bg[0].target1 = bldcnt.target1Bg0;
	renderer->bg[1].target1 = bldcnt.target1Bg1;
	renderer->bg[2].target1 = bldcnt.target1Bg2;
	renderer->bg[3].target1 = bldcnt.target1Bg3;
	renderer->bg[0].target2 = bldcnt.target2Bg0;
	renderer->bg[1].target2 = bldcnt.target2Bg1;
	renderer->bg[2].target2 = bldcnt.target2Bg2;
	renderer->bg[3].target2 = bldcnt.target2Bg3;

	renderer->blendEffect = bldcnt.effect;
	renderer->target1Obj = bldcnt.target1Obj;
	renderer->target1Bd = bldcnt.target1Bd;
	renderer->target2Obj = bldcnt.target2Obj;
	renderer->target2Bd = bldcnt.target2Bd;

	if (oldEffect != renderer->blendEffect) {
		_updatePalettes(renderer);
	}
}

static void _drawScanline(struct GBAVideoSoftwareRenderer* renderer, int y) {
	int i;
	memset(renderer->spriteLayer, 0, sizeof(renderer->spriteLayer));
	if (renderer->dispcnt.objEnable) {
		int j;
		for (j = 0; j < 4; ++j) {
			uint32_t bitmap = renderer->enabledBitmap[j];
			if (!bitmap) {
				continue;
			}
			for (i = j * 16; i < (j + 1) * 16; ++i) {
				if (bitmap & 1) {
					struct GBAObj* sprite = &renderer->d.oam->obj[i];
					if (sprite->transformed) {
						_preprocessTransformedSprite(renderer, &renderer->d.oam->tobj[i], y);
					} else {
						_preprocessSprite(renderer, sprite, y);
					}
				}
				bitmap >>= 1;
			}
		}
	}

	int priority;
	for (priority = 0; priority < 4; ++priority) {
		_postprocessSprite(renderer, priority);
		if (renderer->bg[0].enabled && renderer->bg[0].priority == priority && renderer->dispcnt.mode < 2) {
			_drawBackgroundMode0(renderer, &renderer->bg[0], y);
		}
		if (renderer->bg[1].enabled && renderer->bg[1].priority == priority && renderer->dispcnt.mode < 2) {
			_drawBackgroundMode0(renderer, &renderer->bg[1], y);
		}
		if (renderer->bg[2].enabled && renderer->bg[2].priority == priority) {
			switch (renderer->dispcnt.mode) {
			case 0:
				_drawBackgroundMode0(renderer, &renderer->bg[2], y);
				break;
			case 1:
			case 2:
				_drawBackgroundMode2(renderer, &renderer->bg[2], y);
				break;
			case 3:
				_drawBackgroundMode3(renderer, &renderer->bg[2], y);
				break;
			case 4:
				_drawBackgroundMode4(renderer, &renderer->bg[2], y);
				break;
			case 5:
				_drawBackgroundMode5(renderer, &renderer->bg[2], y);
				break;
			}
			renderer->bg[2].sx += renderer->bg[2].dmx;
			renderer->bg[2].sy += renderer->bg[2].dmy;
		}
		if (renderer->bg[3].enabled && renderer->bg[3].priority == priority) {
			switch (renderer->dispcnt.mode) {
			case 0:
				_drawBackgroundMode0(renderer, &renderer->bg[3], y);
				break;
			case 2:
				_drawBackgroundMode2(renderer, &renderer->bg[3], y);
				break;
			}
			renderer->bg[3].sx += renderer->bg[3].dmx;
			renderer->bg[3].sy += renderer->bg[3].dmy;
		}
	}
}

static void _composite(struct GBAVideoSoftwareRenderer* renderer, int offset, uint32_t color) {
	uint32_t current = renderer->row[offset];
	// We stash the priority on the top bits so we cn do a one-operator comparison
	// The lower the number, the higher the priority, and sprites take precendence over backgrounds
	// We want to do special processing if the color pixel is target 1, however
	if (color < current) {
		if (current & FLAG_UNWRITTEN) {
			renderer->row[offset] = color;
		} else if (!(color & FLAG_TARGET_1) || !(current & FLAG_TARGET_2)) {
			renderer->row[offset] = color | FLAG_FINALIZED;
		} else {
			renderer->row[offset] = _mix(renderer->bldb, current, renderer->blda, color) | FLAG_FINALIZED;
		}
	} else {
		if (current & FLAG_TARGET_1 && color & FLAG_TARGET_2) {
			renderer->row[offset] = _mix(renderer->blda, current, renderer->bldb, color) | FLAG_FINALIZED;
		} else {
			renderer->row[offset] = current | FLAG_FINALIZED;
		}
	}
}

#define BACKGROUND_DRAW_PIXEL_16_NORMAL \
	pixelData = tileData & 0xF; \
	if (pixelData && !(renderer->row[outX] & FLAG_FINALIZED)) { \
		_composite(renderer, outX, renderer->normalPalette[pixelData | paletteData] | flags); \
	} \
	tileData >>= 4;

#define BACKGROUND_DRAW_PIXEL_16_VARIANT \
	pixelData = tileData & 0xF; \
	if (tileData & 0xF && !(renderer->row[outX] & FLAG_FINALIZED)) { \
		_composite(renderer, outX, renderer->variantPalette[pixelData | paletteData] | flags); \
	} \
	tileData >>= 4;

#define BACKGROUND_DRAW_PIXEL_256_NORMAL \
	pixelData = tileData & 0xFF; \
	if (pixelData && !(renderer->row[outX] & FLAG_FINALIZED)) { \
		_composite(renderer, outX, renderer->normalPalette[pixelData] | flags); \
	} \
	tileData >>= 8;

#define BACKGROUND_DRAW_PIXEL_256_VARIANT \
	pixelData = tileData & 0xFF; \
	if (pixelData && !(renderer->row[outX] & FLAG_FINALIZED)) { \
		_composite(renderer, outX, renderer->variantPalette[pixelData] | flags); \
	} \
	tileData >>= 8;

#define BACKGROUND_TEXT_SELECT_CHARACTER \
	localX = tileX * 8 + inX; \
	xBase = localX & 0xF8; \
	if (background->size & 1) { \
		xBase += (localX & 0x100) << 5; \
	} \
	screenBase = (background->screenBase >> 1) + (xBase >> 3) + (yBase << 2); \
	mapData.packed = renderer->d.vram[screenBase]; \
	if (!mapData.vflip) { \
		localY = inY & 0x7; \
	} else { \
		localY = 7 - (inY & 0x7); \
	}

#define BACKGROUND_MODE_0_TILE_16_LOOP(TYPE) \
	uint32_t tileData; \
	int paletteData, pixelData; \
	for (; tileX < 30; ++tileX) { \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		paletteData = mapData.palette << 4; \
		charBase = ((background->charBase + (mapData.tile << 5)) >> 2) + localY; \
		tileData = ((uint32_t*)renderer->d.vram)[charBase]; \
		if (tileData) { \
			if (!mapData.hflip) { \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				++outX; \
			} else { \
				outX += 7; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_16_ ## TYPE; \
				outX += 8; \
			} \
		} else { \
			outX += 8; \
		} \
	}

#define BACKGROUND_MODE_0_TILE_256_LOOP(TYPE) \
	uint32_t tileData; \
	int pixelData; \
	for (; tileX < 30; ++tileX) { \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		charBase = ((background->charBase + (mapData.tile << 6)) >> 2) + (localY << 1); \
		if (!mapData.hflip) { \
			tileData = ((uint32_t*)renderer->d.vram)[charBase]; \
			if (tileData) { \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
			} else { \
				outX += 4; \
			} \
			tileData = ((uint32_t*)renderer->d.vram)[charBase + 1]; \
			if (tileData) { \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
					BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
					++outX; \
			} else { \
				outX += 4; \
			} \
		} else { \
			uint32_t tileData = ((uint32_t*)renderer->d.vram)[charBase + 1]; \
			if (tileData) { \
				outX += 3; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				outX += 4; \
			} else { \
				outX += 4; \
			} \
			tileData = ((uint32_t*)renderer->d.vram)[charBase]; \
			if (tileData) { \
				outX += 3; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				--outX; \
				BACKGROUND_DRAW_PIXEL_256_ ## TYPE; \
				outX += 4; \
			} else { \
				outX += 4; \
			} \
		} \
	}

static void _drawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y) {
	int inX = background->x;
	int inY = y + background->y;
	union GBATextMapData mapData;

	unsigned yBase = inY & 0xF8;
	if (background->size == 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}

	int localX;
	int localY;

	unsigned xBase;

	int flags = (background->priority << OFFSET_PRIORITY) | FLAG_IS_BACKGROUND;
	flags |= FLAG_TARGET_1 * (background->target1 && renderer->blendEffect == BLEND_ALPHA);
	flags |= FLAG_TARGET_2 * background->target2;

	uint32_t screenBase;
	uint32_t charBase;
	int variant = background->target1 && (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);

	int outX = 0;
	int tileX = 0;
	if (inX & 0x7) {
		uint32_t tileData;
		int pixelData, paletteData;
		BACKGROUND_TEXT_SELECT_CHARACTER;

		int end = 0x8 - (inX & 0x7);
		if (!background->multipalette) {
			paletteData = mapData.palette << 4;
			charBase = ((background->charBase + (mapData.tile << 5)) >> 2) + localY;
			tileData = ((uint32_t*)renderer->d.vram)[charBase];
			if (!mapData.hflip) {
				tileData >>= 4 * (inX & 0x7);
				if (!variant) {
					for (outX = 0; outX < end; ++outX) {
						BACKGROUND_DRAW_PIXEL_16_NORMAL;
					}
				} else {
					for (outX = 0; outX < end; ++outX) {
						BACKGROUND_DRAW_PIXEL_16_VARIANT;
					}
				}
			} else {
				if (!variant) {
					for (outX = end; outX--;) {
						BACKGROUND_DRAW_PIXEL_16_NORMAL;
					}
				} else {
					for (outX = end; outX--;) {
						BACKGROUND_DRAW_PIXEL_16_VARIANT;
					}
				}
			}
		} else {
			// TODO: hflip
			charBase = ((background->charBase + (mapData.tile << 6)) >> 2) + (localY << 1);
			outX = 0;
			int end2 = end - 4;
			int shift = inX & 0x3;
			if (end2 > 0) {
				tileData = ((uint32_t*)renderer->d.vram)[charBase];
				tileData >>= 8 * shift;
				shift = 0;
				if (!variant) {
					for (; outX < end2; ++outX) {
						BACKGROUND_DRAW_PIXEL_256_NORMAL;
					}
				} else {
					for (; outX < end2; ++outX) {
						BACKGROUND_DRAW_PIXEL_256_VARIANT;
					}
				}
			}

			tileData = ((uint32_t*)renderer->d.vram)[charBase + 1];
			tileData >>= 8 * shift;
			if (!variant) {
				for (; outX < end; ++outX) {
					BACKGROUND_DRAW_PIXEL_256_NORMAL;
				}
			} else {
				for (; outX < end; ++outX) {
					BACKGROUND_DRAW_PIXEL_256_VARIANT;
				}
			}
		}

		tileX = 30;
		BACKGROUND_TEXT_SELECT_CHARACTER;
		if (!background->multipalette) {
			charBase = ((background->charBase + (mapData.tile << 5)) >> 2) + localY;
			tileData = ((uint32_t*)renderer->d.vram)[charBase];
			paletteData = mapData.palette << 4;
			if (!mapData.hflip) {
				if (!variant) {
					for (outX = VIDEO_HORIZONTAL_PIXELS - 8 + end; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
						BACKGROUND_DRAW_PIXEL_16_NORMAL;
					}
				} else {
					for (outX = VIDEO_HORIZONTAL_PIXELS - 8 + end; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
						BACKGROUND_DRAW_PIXEL_16_VARIANT;
					}
				}
			} else {
				tileData >>= 4 * end;
				if (!variant) {
					for (outX = VIDEO_HORIZONTAL_PIXELS - 1; outX > VIDEO_HORIZONTAL_PIXELS - 8; --outX) {
						BACKGROUND_DRAW_PIXEL_16_NORMAL;
					}
				} else {
					for (outX = VIDEO_HORIZONTAL_PIXELS - 1; outX > VIDEO_HORIZONTAL_PIXELS - 8; --outX) {
						BACKGROUND_DRAW_PIXEL_16_VARIANT;
					}
				}
			}
		} else {
			// TODO: hflip
			charBase = ((background->charBase + (mapData.tile << 6)) >> 2) + (localY << 1);
			outX = VIDEO_HORIZONTAL_PIXELS - 8 + end;
			int end2 = 4 - end;
			if (end2 > 0) {
				tileData = ((uint32_t*)renderer->d.vram)[charBase];
				if (!variant) {
					for (; outX < VIDEO_HORIZONTAL_PIXELS - end2; ++outX) {
						BACKGROUND_DRAW_PIXEL_256_NORMAL;
					}
				} else {
					for (; outX < VIDEO_HORIZONTAL_PIXELS - end2; ++outX) {
						BACKGROUND_DRAW_PIXEL_256_VARIANT;
					}
				}
				++charBase;
			}

			tileData = ((uint32_t*)renderer->d.vram)[charBase];
			if (!variant) {
				for (; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
					BACKGROUND_DRAW_PIXEL_256_NORMAL;
				}
			} else {
				for (; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
					BACKGROUND_DRAW_PIXEL_256_VARIANT;
				}
			}
		}

		tileX = 1;
		outX = end;
	}

	if (!background->multipalette) {
		if (!variant) {
			BACKGROUND_MODE_0_TILE_16_LOOP(NORMAL);
		 } else {
			BACKGROUND_MODE_0_TILE_16_LOOP(VARIANT);
		 }
	} else {
		if (!variant) {
			BACKGROUND_MODE_0_TILE_256_LOOP(NORMAL);
		 } else {
			BACKGROUND_MODE_0_TILE_256_LOOP(VARIANT);
		 }
	}
}

#define BACKGROUND_BITMAP_INIT \
	(void)(unused); \
	int32_t x = background->sx - background->dx; \
	int32_t y = background->sy - background->dy; \
	int32_t localX; \
	int32_t localY; \
	\
	int flags = (background->priority << OFFSET_PRIORITY) | FLAG_IS_BACKGROUND; \
	flags |= FLAG_TARGET_1 * (background->target1 && renderer->blendEffect == BLEND_ALPHA); \
	flags |= FLAG_TARGET_2 * background->target2; \
	int variant = background->target1 && (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);

#define BACKGROUND_BITMAP_ITERATE(W, H) \
	x += background->dx; \
	y += background->dy; \
	\
	if (x < 0 || y < 0 || (x >> 8) >= W || (y >> 8) >= H) { \
		continue; \
	} else { \
		localX = x; \
		localY = y; \
	}

static void _drawBackgroundMode2(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int unused) {
	int sizeAdjusted = 0x8000 << background->size;

	BACKGROUND_BITMAP_INIT;

	uint32_t screenBase = background->screenBase;
	uint32_t charBase = background->charBase;
	uint8_t mapData;
	uint8_t tileData;

	int outX;
	for (outX = 0; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
		x += background->dx;
		y += background->dy;

		if (background->overflow) {
			localX = x & (sizeAdjusted - 1);
			localY = y & (sizeAdjusted - 1);
		} else if (x < 0 || y < 0 || x >= sizeAdjusted || y >= sizeAdjusted) {
			continue;
		} else {
			localX = x;
			localY = y;
		}
		mapData = ((uint8_t*)renderer->d.vram)[screenBase + (localX >> 11) + (((localY >> 7) & 0x7F0) << background->size)];
		tileData = ((uint8_t*)renderer->d.vram)[charBase + (mapData << 6) + ((localY & 0x700) >> 5) + ((localX & 0x700) >> 8)];

		if (tileData && !(renderer->row[outX] & FLAG_FINALIZED)) {
			if (!variant) {
				_composite(renderer, outX, renderer->normalPalette[tileData] | flags);
			} else {
				_composite(renderer, outX, renderer->variantPalette[tileData] | flags);
			}
		}
	}
}

static void _drawBackgroundMode3(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int unused) {
	BACKGROUND_BITMAP_INIT;

	uint16_t color;
	uint32_t color32;

	int outX;
	for (outX = 0; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
		BACKGROUND_BITMAP_ITERATE(VIDEO_HORIZONTAL_PIXELS, VIDEO_VERTICAL_PIXELS);

		color = ((uint16_t*)renderer->d.vram)[(localX >> 8) + (localY >> 8) * VIDEO_HORIZONTAL_PIXELS];
		color32 = 0;
		color32 |= (color << 3) & 0xF8;
		color32 |= (color << 6) & 0xF800;
		color32 |= (color << 9) & 0xF80000;

		if (!(renderer->row[outX] & FLAG_FINALIZED)) {
			if (!variant) {
				_composite(renderer, outX, color32 | flags);
			} else if (renderer->blendEffect == BLEND_BRIGHTEN) {
				_composite(renderer, outX, _brighten(color32, renderer->bldy) | flags);
			} else if (renderer->blendEffect == BLEND_DARKEN) {
				_composite(renderer, outX, _darken(color32, renderer->bldy) | flags);
			}
		}
	}
}

static void _drawBackgroundMode4(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int unused) {
	BACKGROUND_BITMAP_INIT;

	uint16_t color;
	uint32_t offset = 0;
	if (renderer->dispcnt.frameSelect) {
		offset = 0xA000;
	}

	int outX;
	for (outX = 0; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
		BACKGROUND_BITMAP_ITERATE(VIDEO_HORIZONTAL_PIXELS, VIDEO_VERTICAL_PIXELS);

		color = ((uint8_t*)renderer->d.vram)[offset + (localX >> 8) + (localY >> 8) * VIDEO_HORIZONTAL_PIXELS];

		if (color && !(renderer->row[outX] & FLAG_FINALIZED)) {
			if (!variant) {
				_composite(renderer, outX, renderer->normalPalette[color] | flags);
			} else {
				_composite(renderer, outX, renderer->variantPalette[color] | flags);
			}
		}
	}
}

static void _drawBackgroundMode5(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int unused) {
	BACKGROUND_BITMAP_INIT;

	uint16_t color;
	uint32_t color32;
	uint32_t offset = 0;
	if (renderer->dispcnt.frameSelect) {
		offset = 0xA000;
	}

	int outX;
	for (outX = 0; outX < VIDEO_HORIZONTAL_PIXELS; ++outX) {
		BACKGROUND_BITMAP_ITERATE(160, 128);

		color = ((uint16_t*)renderer->d.vram)[(localX >> 8) + (localY >> 8) * 160];
		color32 = 0;
		color32 |= (color << 3) & 0xF8;
		color32 |= (color << 6) & 0xF800;
		color32 |= (color << 9) & 0xF80000;

		if (!(renderer->row[outX] & FLAG_FINALIZED)) {
			if (!variant) {
				_composite(renderer, outX, color32 | flags);
			} else if (renderer->blendEffect == BLEND_BRIGHTEN) {
				_composite(renderer, outX, _brighten(color32, renderer->bldy) | flags);
			} else if (renderer->blendEffect == BLEND_DARKEN) {
				_composite(renderer, outX, _darken(color32, renderer->bldy) | flags);
			}
		}
	}
}

static const int _objSizes[32] = {
	8, 8,
	16, 16,
	32, 32,
	64, 64,
	16, 8,
	32, 8,
	32, 16,
	64, 32,
	8, 16,
	8, 32,
	16, 32,
	32, 64,
	0, 0,
	0, 0,
	0, 0,
	0, 0
};

#define SPRITE_NORMAL_LOOP(DEPTH, TYPE) \
		SPRITE_YBASE_ ## DEPTH(inY); \
		for (int outX = x >= start ? x : start; outX < x + width && outX < end; ++outX) { \
			int inX = outX - x; \
			if (sprite->hflip) { \
				inX = width - inX - 1; \
			} \
			if (!(renderer->row[outX] & FLAG_UNWRITTEN)) { \
				continue; \
			} \
			SPRITE_XBASE_ ## DEPTH(inX); \
			SPRITE_DRAW_PIXEL_ ## DEPTH ## _ ## TYPE(inX); \
		}

#define SPRITE_TRANSFORMED_LOOP(DEPTH, TYPE) \
	for (int outX = x >= start ? x : start; outX < x + totalWidth && outX < end; ++outX) { \
		if (!(renderer->row[outX] & FLAG_UNWRITTEN)) { \
			continue; \
		} \
		int inX = outX - x; \
		int localX = ((mat->a * (inX - (totalWidth >> 1)) + mat->b * (inY - (totalHeight >> 1))) >> 8) + (width >> 1); \
		int localY = ((mat->c * (inX - (totalWidth >> 1)) + mat->d * (inY - (totalHeight >> 1))) >> 8) + (height >> 1); \
		\
		if (localX < 0 || localX >= width || localY < 0 || localY >= height) { \
			continue; \
		} \
		\
		SPRITE_YBASE_ ## DEPTH(localY); \
		SPRITE_XBASE_ ## DEPTH(localX); \
		SPRITE_DRAW_PIXEL_ ## DEPTH ## _ ## TYPE(localX); \
	}

#define SPRITE_XBASE_16(localX) unsigned xBase = (localX & ~0x7) * 4 + ((localX >> 1) & 2);
#define SPRITE_YBASE_16(localY) unsigned yBase = (localY & ~0x7) * (renderer->dispcnt.objCharacterMapping ? width >> 1 : 0x80) + (localY & 0x7) * 4;

#define SPRITE_DRAW_PIXEL_16_NORMAL(localX) \
	uint16_t tileData = renderer->d.vram[(yBase + charBase + xBase) >> 1]; \
	tileData = (tileData >> ((localX & 3) << 2)) & 0xF; \
	if (tileData && !(renderer->spriteLayer[outX])) { \
		renderer->spriteLayer[outX] = renderer->normalPalette[0x100 | tileData | (sprite->palette << 4)] | flags; \
	}

#define SPRITE_DRAW_PIXEL_16_VARIANT(localX) \
	uint16_t tileData = renderer->d.vram[(yBase + charBase + xBase) >> 1]; \
	tileData = (tileData >> ((localX & 3) << 2)) & 0xF; \
	if (tileData && !(renderer->spriteLayer[outX])) { \
		renderer->spriteLayer[outX] = renderer->variantPalette[0x100 | tileData | (sprite->palette << 4)] | flags; \
	}

#define SPRITE_XBASE_256(localX) unsigned xBase = (localX & ~0x7) * 8 + (localX & 6);
#define SPRITE_YBASE_256(localY) unsigned yBase = (localY & ~0x7) * (renderer->dispcnt.objCharacterMapping ? width : 0x80) + (localY & 0x7) * 8;

#define SPRITE_DRAW_PIXEL_256_NORMAL(localX) \
	uint16_t tileData = renderer->d.vram[(yBase + charBase + xBase) >> 1]; \
	tileData = (tileData >> ((localX & 1) << 3)) & 0xFF; \
	if (tileData && !(renderer->spriteLayer[outX])) { \
		renderer->spriteLayer[outX] = renderer->normalPalette[0x100 | tileData] | flags; \
	}

#define SPRITE_DRAW_PIXEL_256_VARIANT(localX) \
	uint16_t tileData = renderer->d.vram[(yBase + charBase + xBase) >> 1]; \
	tileData = (tileData >> ((localX & 1) << 3)) & 0xFF; \
	if (tileData && !(renderer->spriteLayer[outX])) { \
		renderer->spriteLayer[outX] = renderer->variantPalette[0x100 | tileData] | flags; \
	}

static void _preprocessSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBAObj* sprite, int y) {
	int width = _objSizes[sprite->shape * 8 + sprite->size * 2];
	int height = _objSizes[sprite->shape * 8 + sprite->size * 2 + 1];
	int start = renderer->start;
	int end = renderer->end;
	if ((y < sprite->y && (sprite->y + height - 256 < 0 || y >= sprite->y + height - 256)) || y >= sprite->y + height) {
		return;
	}
	int flags = (sprite->priority << OFFSET_PRIORITY) | FLAG_FINALIZED;
	flags |= FLAG_TARGET_1 * ((renderer->target1Obj && renderer->blendEffect == BLEND_ALPHA) || sprite->mode == OBJ_MODE_SEMITRANSPARENT);
	flags |= FLAG_TARGET_2 *renderer->target2Obj;
	int x = sprite->x;
	int inY = y - sprite->y;
	if (sprite->y + height - 256 >= 0) {
		inY += 256;
	}
	if (sprite->vflip) {
		inY = height - inY - 1;
	}
	unsigned charBase = BASE_TILE + sprite->tile * 0x20;
	int variant = renderer->target1Obj && (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
	if (!sprite->multipalette) {
		if (!variant) {
			SPRITE_NORMAL_LOOP(16, NORMAL);
		} else {
			SPRITE_NORMAL_LOOP(16, VARIANT);
		}
	} else {
		if (!variant) {
			SPRITE_NORMAL_LOOP(256, NORMAL);
		} else {
			SPRITE_NORMAL_LOOP(256, VARIANT);
		}
	}
}

static void _preprocessTransformedSprite(struct GBAVideoSoftwareRenderer* renderer, struct GBATransformedObj* sprite, int y) {
	int width = _objSizes[sprite->shape * 8 + sprite->size * 2];
	int totalWidth = width << sprite->doublesize;
	int height = _objSizes[sprite->shape * 8 + sprite->size * 2 + 1];
	int totalHeight = height << sprite->doublesize;
	int start = renderer->start;
	int end = renderer->end;
	if ((y < sprite->y && (sprite->y + totalHeight - 256 < 0 || y >= sprite->y + totalHeight - 256)) || y >= sprite->y + totalHeight) {
		return;
	}
	int flags = (sprite->priority << OFFSET_PRIORITY) | FLAG_FINALIZED;
	flags |= FLAG_TARGET_1 * ((renderer->target1Obj && renderer->blendEffect == BLEND_ALPHA) || sprite->mode == OBJ_MODE_SEMITRANSPARENT);
	flags |= FLAG_TARGET_2 *renderer->target2Obj;
	int x = sprite->x;
	unsigned charBase = BASE_TILE + sprite->tile * 0x20;
	struct GBAOAMMatrix* mat = &renderer->d.oam->mat[sprite->matIndex];
	int variant = renderer->target1Obj && (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
	int inY = y - sprite->y;
	if (inY < 0) {
		inY += 256;
	}
	if (!sprite->multipalette) {
		if (!variant) {
			SPRITE_TRANSFORMED_LOOP(16, NORMAL);
		} else {
			SPRITE_TRANSFORMED_LOOP(16, VARIANT);
		}
	} else {
		if (!variant) {
			SPRITE_TRANSFORMED_LOOP(256, NORMAL);
		} else {
			SPRITE_TRANSFORMED_LOOP(256, VARIANT);
		}
	}
}

static void _postprocessSprite(struct GBAVideoSoftwareRenderer* renderer, int priority) {
	int x;
	for (x = 0; x < 240; ++x) {
		uint32_t color = renderer->spriteLayer[x];
		if ((color & FLAG_FINALIZED) && (color & FLAG_PRIORITY) >> OFFSET_PRIORITY == priority && !(renderer->row[x] & FLAG_FINALIZED)) {
			_composite(renderer, x, color & ~FLAG_FINALIZED);
		}
	}
}

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer) {
	if (renderer->blendEffect == BLEND_BRIGHTEN) {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _brighten(renderer->normalPalette[i], renderer->bldy);
		}
	} else if (renderer->blendEffect == BLEND_DARKEN) {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _darken(renderer->normalPalette[i], renderer->bldy);
		}
	} else {
		for (int i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = renderer->normalPalette[i];
		}
	}
}

static inline uint32_t _brighten(uint32_t color, int y) {
	uint32_t c = 0;
	uint32_t a;
	a = color & 0xF8;
	c |= (a + ((0xF8 - a) * y) / 16) & 0xF8;

	a = color & 0xF800;
	c |= (a + ((0xF800 - a) * y) / 16) & 0xF800;

	a = color & 0xF80000;
	c |= (a + ((0xF80000 - a) * y) / 16) & 0xF80000;
	return c;
}

static inline uint32_t _darken(uint32_t color, int y) {
	uint32_t c = 0;
	uint32_t a;
	a = color & 0xF8;
	c |= (a - (a * y) / 16) & 0xF8;

	a = color & 0xF800;
	c |= (a - (a * y) / 16) & 0xF800;

	a = color & 0xF80000;
	c |= (a - (a * y) / 16) & 0xF80000;
	return c;
}

static uint32_t _mix(int weightA, uint32_t colorA, int weightB, uint32_t colorB) {
	uint32_t c = 0;
	uint32_t a, b;
	a = colorA & 0xF8;
	b = colorB & 0xF8;
	c |= ((a * weightA + b * weightB) / 16) & 0x1F8;
	if (c & 0x00000100) {
		c = 0x000000F8;
	}

	a = colorA & 0xF800;
	b = colorB & 0xF800;
	c |= ((a * weightA + b * weightB) / 16) & 0x1F800;
	if (c & 0x00010000) {
		c = (c & 0x000000F8) | 0x0000F800;
	}

	a = colorA & 0xF80000;
	b = colorB & 0xF80000;
	c |= ((a * weightA + b * weightB) / 16) & 0x1F80000;
	if (c & 0x01000000) {
		c = (c & 0x0000F8F8) | 0x00F80000;
	}
	return c;
}
