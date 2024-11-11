#include "gba/types.h"
#include "gba/defines.h"
#include "gba/macro.h"
#include "global.h"
#include "bg.h"
#include "data.h"
#include "decompress.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_weather.h"
#include "gpu_regs.h"
#include "graphics.h"
#include "malloc.h"
#include "main.h"
#include "menu_helpers.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "scanline_effect.h"
#include "script.h"
#include "sound.h"
#include "sprite.h"
#include "task.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/rgb.h"
#include "constants/songs.h"

enum Icon
{
    ICON_ERROR,
    ICON_NONE = 0xFF
};

struct TechDiffState
{
    MainCallback savedCallback;
    u8 loadState;
};

static EWRAM_DATA struct TechDiffState *sTechDiffState = NULL; 
static EWRAM_DATA u8 *sBg1TilemapBuffer = NULL;

// Coords for error icon
#define ERROR_ICON_X    116
#define ERROR_ICON_Y    83

static const struct BgTemplate sTechDiffBgTemplates[] = 
{
    {
        .bg = 0,
        .charBaseIndex = 3,
        .mapBaseIndex = 30,
        .priority = 2
    }
};

static const u32 sTechDiffTiles[] = INCBIN_U32("graphics/technical_difficulties/techdifftiles.4bpp.lz");

static const u32 sTechDiffErrorIcon[] = INCBIN_U32("graphics/technical_difficulties/error.4bpp");

static const u32 sTechDiffTilemap[] = INCBIN_U32("graphics/technical_difficulties/techdifftilemap.bin.lz");

static const u16 sTechDiffBgPalette[] = INCBIN_U16("graphics/technical_difficulties/techdiff.gbapal");
static const u16 sTechDiffErrorIconPalette[] = INCBIN_U16("graphics/technical_difficulties/error.gbapal");

#define PALETTE_TAG_ERROR_ICON 0x1000
static const struct SpritePalette sErrorIconSpritePalette = 
{
    .data = sTechDiffErrorIconPalette,
    .tag = PALETTE_TAG_ERROR_ICON
};

#define DEFAULT_ANIM 0
#define SELECTED_ANIM 0

static const union AnimCmd sErrorIconDefaultAnim[] = 
{
    ANIMCMD_FRAME(0, 30),
    ANIMCMD_END
};

static const union AnimCmd *const sErrorIconAnims[] = 
{
    [DEFAULT_ANIM] = sErrorIconDefaultAnim
};

static const struct SpriteFrameImage sErrorIconPicTable[] = 
{
    obj_frame_tiles(sTechDiffErrorIcon)
};

static const struct OamData sErrorIconOam = 
{
    .y = ERROR_ICON_Y,
    .affineMode = ST_OAM_AFFINE_OFF,
    .objMode = ST_OAM_OBJ_NORMAL,
    .mosaic = FALSE,
    .bpp = ST_OAM_4BPP,
    .shape = SPRITE_SHAPE(32x32),
    .x = ERROR_ICON_X,
    .matrixNum = 0,
    .size = SPRITE_SIZE(32x32),
    .tileNum = 0,
    .priority = 0,
    .paletteNum = 0,
    .affineParam = 0,
};

static const struct SpriteTemplate sErrorIconSpriteTemplate = 
{
    .tileTag = TAG_NONE,
    .paletteTag = PALETTE_TAG_ERROR_ICON,
    .oam = &sErrorIconOam,
    .anims = sErrorIconAnims,
    .images = sErrorIconPicTable,
    .callback = SpriteCallbackDummy,
};

// Callbacks
static void TechDiff_SetupCB(void);
static void TechDiff_MainCB(void);
static void TechDiff_VBlankCB(void);

// Tasks
static void Task_TechDiffWaitFadeIn(u8 taskId);
static void Task_TechDiffMainInput(u8 taskId);
static void Task_TechDiffWaitFadeAndBail(u8 taskId);
static void Task_TechDiffWaitFadeAndExitGracefully(u8 taskId);

// Helper functions
static void TechDiff_Init(MainCallback callback);
static bool8 TechDiff_InitBgs(void);
static void TechDiff_FadeAndBail(void);
static bool8 TechDiff_LoadGraphics(void);
static void TechDiff_CreateErrorIcon(void);
static void TechDiff_FreeResources(void);

// Task that is activated by the overworld special "techdiff"
static void Task_OpenTechDiff(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        CleanupOverworldWindowsAndTilemaps();
        TechDiff_Init(CB2_ReturnToField);
        DestroyTask(taskId);
    }
}

// "techdiff" overworld special
void TechDiff(void)
{
    LockPlayerFieldControls();
    CreateTask(Task_OpenTechDiff, 0);
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
}

static void TechDiff_Init(MainCallback callback)
{
    sTechDiffState = AllocZeroed(sizeof(struct TechDiffState));
    if (sTechDiffState == NULL)
    {
        SetMainCallback2(callback);
        return;
    }

    sTechDiffState->loadState = 0;
    sTechDiffState->savedCallback = callback;

    SetMainCallback2(TechDiff_SetupCB);
}

static void TechDiff_SetupCB(void)
{
    switch (gMain.state)
    {
    case 0:
        DmaClearLarge16(3, (void *)VRAM, VRAM_SIZE, 0x1000);
        SetVBlankHBlankCallbacksToNull();
        ClearScheduledBgCopiesToVram();
        gMain.state++;
        break;
    case 1:
        ScanlineEffect_Stop();
        FreeAllSpritePalettes();
        ResetPaletteFade();
        ResetSpriteData();
        ResetTasks();
        InitMapMusic();
        ResetMapMusic();
        gMain.state++;
        break;
    case 2:
        if (TechDiff_InitBgs())
        {
            sTechDiffState->loadState = 0;
            gMain.state++;
        }
        else
        {
            TechDiff_FadeAndBail();
            return;
        }
        break;
    case 3:
        if (TechDiff_LoadGraphics() == TRUE)
        {
            gMain.state++;
        }
        break;
    case 4:
        TechDiff_CreateErrorIcon();
        CreateTask(Task_TechDiffWaitFadeIn, 0);
        gMain.state++;
        break;
    case 5:
        BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
        StopMapMusic();
        PlayFanfare(MUS_TOO_BAD);
        gMain.state++;
        break;
    case 6:
        SetVBlankCallback(TechDiff_VBlankCB);
        SetMainCallback2(TechDiff_MainCB);
        break;
    }   
}

static void TechDiff_MainCB(void)
{
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    DoScheduledBgTilemapCopiesToVram();
    UpdatePaletteFade();
    MapMusicMain();
}

static void TechDiff_VBlankCB(void)
{
    LoadOam();
    ProcessSpriteCopyRequests();
    TransferPlttBuffer();
}

static void Task_TechDiffWaitFadeIn(u8 taskId)
{
    if(!gPaletteFade.active)
    {
        gTasks[taskId].func = Task_TechDiffMainInput;
    }
}

static void Task_TechDiffMainInput(u8 taskId)
{
    if (JOY_NEW(B_BUTTON))
    {
        PlaySE(SE_SELECT);
        BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
        gTasks[taskId].func = Task_TechDiffWaitFadeAndExitGracefully;
    }
}

static void Task_TechDiffWaitFadeAndBail(u8 taskId)
{
    if(!gPaletteFade.active)
    {
        SetMainCallback2(sTechDiffState->savedCallback);
        TechDiff_FreeResources();
        DestroyTask(taskId);
    }
}

static void Task_TechDiffWaitFadeAndExitGracefully(u8 taskId)
{
    if (!gPaletteFade.active)
    {
        SetMainCallback2(sTechDiffState->savedCallback);
        TechDiff_FreeResources();
        DestroyTask(taskId);
    }
}
#define TILEMAP_BUFFER_SIZE (1024 * 2)
static bool8 TechDiff_InitBgs(void)
{
    ResetAllBgsCoordinates();

    sBg1TilemapBuffer = AllocZeroed(TILEMAP_BUFFER_SIZE);
    if (sBg1TilemapBuffer == NULL)
    {
        return FALSE;
    }
    ResetBgsAndClearDma3BusyFlags(0);
    InitBgsFromTemplates(0, sTechDiffBgTemplates, NELEMS(sTechDiffBgTemplates));
    SetBgTilemapBuffer(0, sBg1TilemapBuffer);
    ScheduleBgCopyTilemapToVram(0);

    ShowBg(0);

    return TRUE;
}

static void TechDiff_FadeAndBail(void)
{
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
    CreateTask(Task_TechDiffWaitFadeAndBail, 0);
    SetVBlankCallback(TechDiff_VBlankCB);
    SetMainCallback2(TechDiff_MainCB);    
}

static bool8 TechDiff_LoadGraphics(void)
{
    switch (sTechDiffState->loadState)
    {
    case 0:
        ResetTempTileDataBuffers();
        DecompressAndCopyTileDataToVram(0, sTechDiffTiles, 0, 0, 0);
        sTechDiffState->loadState++;
        break;
    case 1:
        if(FreeTempTileDataBuffersIfPossible() != TRUE)
        {
            LZDecompressWram(sTechDiffTilemap, sBg1TilemapBuffer);
            sTechDiffState->loadState++;
        }
        break;
    case 2:
        LoadPalette(sTechDiffBgPalette, BG_PLTT_ID(0), PLTT_SIZE_4BPP);
        sTechDiffState->loadState++;
    default:
        sTechDiffState->loadState = 0;
        return TRUE;
    }
    return FALSE;
}

static void TechDiff_CreateErrorIcon(void)
{
    LoadSpritePalette(&sErrorIconSpritePalette);
    CreateSprite(&sErrorIconSpriteTemplate, ERROR_ICON_X, ERROR_ICON_Y, 0);
}

static void TechDiff_FreeResources(void)
{
    if (sTechDiffState != NULL)
    {
        Free(sTechDiffState);
    }
    if (sBg1TilemapBuffer != NULL)
    {
        Free(sBg1TilemapBuffer);
    }
    FreeAllWindowBuffers();
    ResetSpriteData();
}