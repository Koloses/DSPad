/*
 * DSPad - XInput controller support for Dungeon Siege: Legends of Aranna (DSLOA.exe 1.50)
 *
 * Built as Mss32.dll, a proxy that forwards every Miles Sound System export to the game's
 * original library (renamed mss32o.dll). Hooks PeekMessageA in the game's import table to get
 * a per-frame callback on the main thread, then drives the game through the engine's own
 * exported functions (GoMind::RSMove etc.), a few patched call sites, and synthesized
 * mouse/keyboard input.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

void proxy_init(void);

/* ------------------------------------------------------------------ types */
typedef struct { float x, y, z; unsigned node; } SiegePos;
typedef struct { float x, y, z; } Vec3;
typedef struct { void **b, **e, **c; } GopColl;
typedef struct { int l, t, r, b; } GRect;
typedef unsigned char bool8;
#define TC __attribute__((thiscall))

enum { JAT_ATTACK_OBJECT = 3, JAT_CAST = 10, JAT_FOLLOW = 20, JAT_MOVE = 26 };
enum { JQ_ACTION = 1 };
enum { QP_CLEAR = 5 };
enum { AO_HUMAN = 1 };

/* ------------------------------------------------------------------ XInput */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGAMEPAD;
typedef struct { DWORD packet; XGAMEPAD pad; } XSTATE;
static DWORD (WINAPI *XGetState)(DWORD, XSTATE *);
enum {
    B_UP = 0x0001, B_DOWN = 0x0002, B_LEFT = 0x0004, B_RIGHT = 0x0008,
    B_START = 0x0010, B_BACK = 0x0020, B_L3 = 0x0040, B_R3 = 0x0080,
    B_LB = 0x0100, B_RB = 0x0200, B_A = 0x1000, B_B = 0x2000, B_X = 0x4000, B_Y = 0x8000,
    B_LT = 0x10000, B_RT = 0x20000,
    N_UP = 0x100000, N_DOWN = 0x200000, N_LEFT = 0x400000, N_RIGHT = 0x800000 /* left stick as dpad */
};

/* ------------------------------------------------------------------ log */
static char g_dir[MAX_PATH];
static char g_ini[MAX_PATH];
static int g_logLevel = 1;
static void logf_(const char *fmt, ...)
{
    if (!g_logLevel) return;
    char line[1024], path[MAX_PATH];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); n += vsnprintf(line + n, sizeof line - n - 3, fmt, ap); va_end(ap);
    if (n > (int)sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n';
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) snprintf(path, sizeof path, "%sdspad.log", g_dir);
        else { DWORD l = GetTempPathA(MAX_PATH - 16, path); lstrcpyA(path + l, "dspad.log"); }
        HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD w; WriteFile(h, line, n, &w, NULL); CloseHandle(h);
    }
    OutputDebugStringA(line);
}

static int rd(const void *p, size_t n) { return p && !IsBadReadPtr(p, n); }

/* ------------------------------------------------------------------ engine symbols */
static HMODULE g_exe;
static int g_missing;
static void *sym(const char *n)
{
    void *p = (void *)GetProcAddress(g_exe, n);
    if (!p) { g_missing++; logf_("missing export: %s", n); }
    return p;
}

static void *(*Server_Get)(void);
static void *(*GoDb_Get)(void);
static void *(*AIQuery_Get)(void);
static void *(*UIShell_Get)(void);
static void *(*AppModule_Get)(void);
static bool8 (TC *AppModule_IsUserPaused)(void *);
static int (TC *AppModule_GetCursorX)(void *);
static int (TC *AppModule_GetCursorY)(void *);
static void *(*UIGame_Get)(void);
static void *(*UICommands_Get)(void);
static void *(*WorldState_Get)(void);
static void *(*SiegeEngine_Get)(void);

static void *(TC *Server_GetScreenParty)(void *);
static void *(TC *Server_GetScreenHero)(void *);
static void *(TC *GoDb_GetFocusGo)(void *);

static GopColl *(TC *Go_GetChildren)(void *);
static bool8 (TC *Go_IsSelected)(void *);
static void *(TC *Go_GetGoid)(void *);
static void *(TC *Go_GetMind)(void *);
static void *(TC *Go_GetPlacement)(void *);
static void *(TC *Go_GetInventory)(void *);
static void *(TC *Go_GetMagic)(void *);
static int (TC *Go_GetLifeState)(void *);
static bool8 (TC *Go_IsItem)(void *);
static bool8 (TC *Go_IsInsideInventory)(void *);
static bool8 (TC *Go_IsActor)(void *);
static bool8 (TC *Go_IsUsable)(void *);
static bool8 (TC *Go_IsSpell)(void *);
static bool8 (TC *Go_HasConversation)(void *);
static bool8 (TC *Go_HasStore)(void *);
static bool8 (TC *Go_IsScreenPartyMember)(void *);
static bool8 (*LS_IsAlive)(int);

static SiegePos *(TC *GoPlacement_GetPosition)(void *);

static void (TC *GoMind_RSMove)(void *, const SiegePos *, int, int);
static void (TC *GoMind_RSStop)(void *, int);
static void (TC *GoMind_RSGet)(void *, void *, int, int);
static void (TC *GoMind_RSUse)(void *, void *, int, int);
static void (TC *GoMind_RSDoJob)(void *, void *);
static void (TC *GoMind_RSDrinkLife)(void *, int);
static void (TC *GoMind_RSDrinkMana)(void *, int);
static bool8 (TC *GoMind_GetEnemiesInSphere)(void *, float, GopColl *);
static bool8 (TC *GoMind_IsEnemy)(void *, void *);
static void *(TC *GoMind_GetFrontJob)(void *, int);
static bool8 (TC *AIQuery_GetOccupantsInSphere)(void *, const SiegePos *, float, GopColl *);

static void *(*MakeJobReq_G)(int, int, int, int, void *);
static void *(*MakeJobReq_GG)(int, int, int, int, void *, void *);
static void (TC *JobReq_SetFloat1)(void *, float);

static void *(TC *GoInventory_GetSelectedItem)(void *);
static bool8 (TC *GoMagic_IsCastableOn)(void *, void *, bool8);
static void (TC *UICommands_RSTalk)(void *, void *, void *, bool8);
static bool8 (TC *UIGame_SelectNextPlayer)(void *);
static bool8 (TC *UIGame_SelectLastPlayer)(void *);

static void *(TC *SiegeEngine_GetCamera)(void *);
static SiegePos *(TC *Cam_GetCameraSiegePos)(void *);
static SiegePos *(TC *Cam_GetTargetSiegePos)(void *);
static Vec3 *(TC *Cam_GetCameraPosition)(void *);
static Vec3 *(TC *Cam_GetTargetPosition)(void *);
static float *(TC *Cam_GetMatrixOrientation)(void *);
static void (*GetSiegeDifference)(Vec3 *, const SiegePos *, const SiegePos *);
static bool8 (TC *Engine_AdjustPointToTerrain)(void *, SiegePos *, DWORD, int, Vec3 *);

static int (TC *WorldState_GetCurrentState)(void *);
static const char *(*WS_ToString)(int);
static const char *(*UIType_ToString)(int);
static int (TC *UIShell_GetScreenWidth)(void *);
static int (TC *UIShell_GetScreenHeight)(void *);
static int (TC *UIWindow_GetType)(void *);
static GRect *(TC *UIWindow_GetRect)(void *);
static bool8 (TC *UIWindow_GetVisible)(void *);
static bool8 (TC *UIWindow_IsEnabled)(void *);
static void *(TC *UIWindow_GetChildren)(void *);
static int (TC *UIWindowVec_Size)(void *);
static void *(TC *UIWindowVec_Get)(void *, int);
static int (TC *UIGridbox_GetBoxWidth)(void *);
static int (TC *UIGridbox_GetBoxHeight)(void *);
static int (TC *UIGridbox_GetColumns)(void *);
static int (TC *UIGridbox_GetRows)(void *);

static void *(*MCP_Get)(void);
static int (TC *MCP_MakeRequest)(void *, void *, int, const SiegePos *, float);
static bool8 (*MCPReq_FromString)(const char *, int *);
static const char *(*MCPRet_ToString)(int);
static void *(*World_Get)(void);
static bool8 (TC *World_IsMultiPlayer)(void *);
static void *(TC *Go_GetAspect)(void *);
static char *(TC *GoAspect_GetAspectPtr)(void *);
static bool8 (TC *MCP_Flush)(void *, void *, float);
static int g_plApproach = -1, g_plFace = -1;
static int (TC *MCP_MakeRequestRot)(void *, void *, int, const void *);
/* direct hero control */
enum { CHORE_DEFAULT = 3, CHORE_WALK = 4 };
static void *(TC *Go_GetBody)(void *);
static bool8 (TC *Go_IsBreakable)(void *);
static void (TC *GoBody_SAnimate)(void *, void *);
static void *(*MakeAnimReq)(int);
static float (TC *GoBody_GetAvgMoveVelocity)(void *);
static int (TC *GoBody_GetPerm)(void *);
static bool8 (TC *AIQuery_IsAreaWalkable)(void *, int, const SiegePos *, unsigned, float);
static void (TC *GoPlacement_SSetPosition)(void *, const SiegePos *, bool8);
static void (TC *GoPlacement_OrientToPosition)(void *, const SiegePos *);
static int (TC *Aspect_GetCurrentChore)(void *);
static int (TC *Aspect_GetNextChore)(void *);
static float (TC *Aspect_GetCurrentVelocity)(void *);
static void (TC *GoMind_ClearQ)(void *, int);
static char *(TC *Go_GetFollower)(void *);
static bool8 (TC *GoAspect_GetIsSelectable)(void *);
static bool8 (TC *GoMind_IsLosClear)(void *, void *);
static bool8 (TC *GoAspect_GetIsVisible)(void *);
static bool8 (TC *Go_IsRangedWeapon)(void *);
static void (TC *GoMind_SetMayAttack)(void *, bool8);
static float (TC *GoMind_GetWeaponRange)(void *);
static float (TC *GoMind_GetMeleeEngageRange)(void *);
static float (TC *GoMind_GetPersonalSpaceRange)(void *);
static bool8 (TC *GoMind_GetAutoFidgets)(void *);
static void (TC *GoMind_SetAutoFidgets)(void *, bool8);
static void *(TC *Aspect_GetBlender)(void *);
static int (TC *Aspect_GetCurrentStance)(void *);
static float (TC *Blender_GetBaseDuration)(void *, int, int);
static float *(TC *GoPlacement_GetOrientation)(void *);
static int g_directOK;
static int g_gameOK, g_uiOK;
static void **g_mapNil; /* VC6 std::map _Nil sentinel used by the UI window maps */

static void resolve(void)
{
    g_exe = GetModuleHandleA(NULL);
#define S(var, name) var = sym(name)
    g_missing = 0;
    S(Server_Get, "?FUBI_GetClassSingleton@Server@@CAPAV1@XZ");
    S(GoDb_Get, "?FUBI_GetClassSingleton@GoDb@@CAPAV1@XZ");
    S(AIQuery_Get, "?FUBI_GetClassSingleton@AIQuery@@CAPAV1@XZ");
    S(UIGame_Get, "?FUBI_GetClassSingleton@UIGame@@CAPAV1@XZ");
    S(UICommands_Get, "?FUBI_GetClassSingleton@UICommands@@CAPAV1@XZ");
    S(SiegeEngine_Get, "?FUBI_GetClassSingleton@SiegeEngine@siege@@CAPAV12@XZ");
    S(Server_GetScreenParty, "?GetScreenParty@Server@@QAEPAVGo@@XZ");
    S(Server_GetScreenHero, "?GetScreenHero@Server@@QAEPAVGo@@XZ");
    S(GoDb_GetFocusGo, "?GetFocusGo@GoDb@@QBEPBUGoid_@@XZ");
    S(Go_GetChildren, "?GetChildren@Go@@QBEABUGopColl@@XZ");
    S(Go_IsSelected, "?IsSelected@Go@@QBE_NXZ");
    S(Go_GetGoid, "?GetGoid@Go@@QBEPBUGoid_@@XZ");
    S(Go_GetMind, "?GetMind@Go@@QAEPAVGoMind@@XZ");
    S(Go_GetPlacement, "?GetPlacement@Go@@QAEPAVGoPlacement@@XZ");
    S(Go_GetInventory, "?GetInventory@Go@@QAEPAVGoInventory@@XZ");
    S(Go_GetMagic, "?GetMagic@Go@@QAEPAVGoMagic@@XZ");
    S(Go_GetLifeState, "?GetLifeState@Go@@QBE?AW4eLifeState@@XZ");
    S(Go_IsItem, "?IsItem@Go@@QBE_NXZ");
    S(Go_IsInsideInventory, "?IsInsideInventory@Go@@QBE_NXZ");
    S(Go_IsActor, "?IsActor@Go@@QBE_NXZ");
    S(Go_IsUsable, "?IsUsable@Go@@QBE_NXZ");
    S(Go_IsSpell, "?IsSpell@Go@@QBE_NXZ");
    S(Go_HasConversation, "?HasConversation@Go@@QBE_NXZ");
    S(Go_HasStore, "?HasStore@Go@@QBE_NXZ");
    S(Go_IsScreenPartyMember, "?IsScreenPartyMember@Go@@QBE_NXZ");
    S(LS_IsAlive, "?IsAlive@@YA_NW4eLifeState@@@Z");
    S(GoPlacement_GetPosition, "?GetPosition@GoPlacement@@QBEABUSiegePos@@XZ");
    S(GoMind_RSMove, "?RSMove@GoMind@@QAEXABUSiegePos@@W4eQPlace@@W4eActionOrigin@@@Z");
    S(GoMind_RSStop, "?RSStop@GoMind@@QAEXW4eActionOrigin@@@Z");
    S(GoMind_RSGet, "?RSGet@GoMind@@QAEXPAVGo@@W4eQPlace@@W4eActionOrigin@@@Z");
    S(GoMind_RSUse, "?RSUse@GoMind@@QAEXPAVGo@@W4eQPlace@@W4eActionOrigin@@@Z");
    S(GoMind_RSDoJob, "?RSDoJob@GoMind@@QAEXABUJobReq@@@Z");
    S(GoMind_RSDrinkLife, "?RSDrinkLifeHealingPotion@GoMind@@QAEXW4eActionOrigin@@@Z");
    S(GoMind_RSDrinkMana, "?RSDrinkManaHealingPotion@GoMind@@QAEXW4eActionOrigin@@@Z");
    S(GoMind_GetEnemiesInSphere, "?GetEnemiesInSphere@GoMind@@QBE_NMPAUGopColl@@@Z");
    S(GoMind_IsEnemy, "?IsEnemy@GoMind@@QBE_NPBVGo@@@Z");
    S(GoMind_GetFrontJob, "?GetFrontJob@GoMind@@QBEPAVJob@@W4eJobQ@@@Z");
    S(AIQuery_GetOccupantsInSphere, "?GetOccupantsInSphere@AIQuery@@QAE_NABUSiegePos@@MAAUGopColl@@@Z");
    S(MakeJobReq_G, "?MakeJobReq@@YAAAUJobReq@@W4eJobAbstractType@@W4eJobQ@@W4eQPlace@@W4eActionOrigin@@PBUGoid_@@@Z");
    S(MakeJobReq_GG, "?MakeJobReq@@YAAAUJobReq@@W4eJobAbstractType@@W4eJobQ@@W4eQPlace@@W4eActionOrigin@@PBUGoid_@@4@Z");
    S(JobReq_SetFloat1, "?SetFloat1@JobReq@@QAEXM@Z");
    S(GoInventory_GetSelectedItem, "?GetSelectedItem@GoInventory@@QBEPAVGo@@XZ");
    S(GoMagic_IsCastableOn, "?IsCastableOn@GoMagic@@QAE_NPAVGo@@_N@Z");
    S(UICommands_RSTalk, "?RSTalk@UICommands@@QAEXPAVGo@@0_N@Z");
    S(UIGame_SelectNextPlayer, "?SelectNextPlayer@UIGame@@QAE_NXZ");
    S(UIGame_SelectLastPlayer, "?SelectLastPlayer@UIGame@@QAE_NXZ");
    S(SiegeEngine_GetCamera, "?GetCamera@SiegeEngine@siege@@QBEABVSiegeCamera@2@XZ");
    S(Cam_GetCameraSiegePos, "?GetCameraSiegePos@SiegeCamera@siege@@QBEABUSiegePos@@XZ");
    S(Cam_GetTargetSiegePos, "?GetTargetSiegePos@SiegeCamera@siege@@QBEABUSiegePos@@XZ");
    S(Cam_GetCameraPosition, "?GetCameraPosition@SiegeCamera@siege@@QBEABUvector_3@@XZ");
    S(Cam_GetTargetPosition, "?GetTargetPosition@SiegeCamera@siege@@QBEABUvector_3@@XZ");
    S(Cam_GetMatrixOrientation, "?GetMatrixOrientation@SiegeCamera@siege@@QAEABUmatrix_3x3@@XZ");
    S(GetSiegeDifference, "?GetSiegeDifference@@YAXAAUvector_3@@ABUSiegePos@@1@Z");
    g_gameOK = (g_missing == 0);

    /* optional extras */
    MCP_Get = (void *)GetProcAddress(g_exe, "?FUBI_GetClassSingleton@Manager@MCP@@CAPAV12@XZ");
    MCP_MakeRequest = (void *)GetProcAddress(g_exe, "?MakeRequest@Manager@MCP@@QAE?AW4eReqRetCode@2@PBUGoid_@@W4eRequest@2@ABUSiegePos@@M@Z");
    MCP_Flush = (void *)GetProcAddress(g_exe, "?Flush@Manager@MCP@@QAE_NPBUGoid_@@M@Z");
    MCPReq_FromString = (void *)GetProcAddress(g_exe, "?FromString@@YA_NPBDAAW4eRequest@MCP@@@Z");
    MCPRet_ToString = (void *)GetProcAddress(g_exe, "?ToString@@YAPBDW4eReqRetCode@MCP@@@Z");
    World_Get = (void *)GetProcAddress(g_exe, "?FUBI_GetClassSingleton@World@@CAPAV1@XZ");
    World_IsMultiPlayer = (void *)GetProcAddress(g_exe, "?IsMultiPlayer@World@@QBE_NXZ");
    Go_GetAspect = (void *)GetProcAddress(g_exe, "?GetAspect@Go@@QAEPAVGoAspect@@XZ");
    GoAspect_GetAspectPtr = (void *)GetProcAddress(g_exe, "?GetAspectPtr@GoAspect@@QBEPAVAspect@nema@@XZ");
    if (MCP_Get && MCP_MakeRequest && MCP_Flush && MCPReq_FromString && MCPRet_ToString && World_Get && World_IsMultiPlayer) {
        int v = -1;
        if (MCPReq_FromString("pl_approach", &v) || MCPReq_FromString("PL_APPROACH", &v)) g_plApproach = v;
        MCP_MakeRequestRot = (void *)GetProcAddress(g_exe, "?MakeRequest@Manager@MCP@@QAE?AW4eReqRetCode@2@PBUGoid_@@W4eRequest@2@ABUSiegeRot@@@Z");
        v = -1;
        if (MCP_MakeRequestRot && (MCPReq_FromString("pl_face", &v) || MCPReq_FromString("PL_FACE", &v))) g_plFace = v;
    }
#define O(var, name) var = (void *)GetProcAddress(g_exe, name)
    O(Go_GetBody, "?GetBody@Go@@QAEPAVGoBody@@XZ");
    O(Go_IsBreakable, "?IsBreakable@Go@@QBE_NXZ");
    O(GoBody_SAnimate, "?SAnimate@GoBody@@QAEXABUAnimReq@@@Z");
    O(MakeAnimReq, "?MakeAnimReq@@YAAAUAnimReq@@W4eAnimChore@@@Z");
    O(GoBody_GetAvgMoveVelocity, "?GetAvgMoveVelocity@GoBody@@QAEMXZ");
    O(GoBody_GetPerm, "?GetTerrainMovementPermissions@GoBody@@QBE?AW4eLogicalNodeFlags@siege@@XZ");
    O(AIQuery_IsAreaWalkable, "?IsAreaWalkable@AIQuery@@QAE_NW4eLogicalNodeFlags@siege@@ABUSiegePos@@KM@Z");
    O(GoPlacement_SSetPosition, "?SSetPosition@GoPlacement@@QAEXABUSiegePos@@_N@Z");
    O(GoPlacement_OrientToPosition, "?OrientToPosition@GoPlacement@@QAEXABUSiegePos@@@Z");
    O(Aspect_GetCurrentChore, "?GetCurrentChore@Aspect@nema@@QBE?AW4eAnimChore@@XZ");
    O(Aspect_GetNextChore, "?GetNextChore@Aspect@nema@@QBE?AW4eAnimChore@@XZ");
    O(Aspect_GetCurrentVelocity, "?GetCurrentVelocity@Aspect@nema@@QBE?BMXZ");
    O(GoMind_ClearQ, "?Clear@GoMind@@QAEXW4eJobQ@@@Z");
    O(Go_IsRangedWeapon, "?IsRangedWeapon@Go@@QBE_NXZ");
    O(GoMind_SetMayAttack, "?SetMayAttack@GoMind@@QAEX_N@Z");
    O(GoMind_GetAutoFidgets, "?GetActorAutoFidgets@GoMind@@QAE_NXZ");
    O(GoMind_SetAutoFidgets, "?SetActorAutoFidgets@GoMind@@QAEX_N@Z");
    O(GoMind_GetMeleeEngageRange, "?GetMeleeEngageRange@GoMind@@QBEMXZ");
    O(GoMind_GetPersonalSpaceRange, "?GetPersonalSpaceRange@GoMind@@QBEMXZ");
    O(GoMind_GetWeaponRange, "?GetWeaponRange@GoMind@@QBEMXZ");
    O(Aspect_GetBlender, "?GetBlender@Aspect@nema@@QAEPAVBlender@2@XZ");
    O(Aspect_GetCurrentStance, "?GetCurrentStance@Aspect@nema@@QBE?AW4eAnimStance@@XZ");
    O(Blender_GetBaseDuration, "?GetBaseDuration@Blender@nema@@QBEMW4eAnimChore@@W4eAnimStance@@@Z");
    O(GoMind_IsLosClear, "?IsLosClear@GoMind@@QBE_NPAVGo@@@Z");
    O(GoAspect_GetIsSelectable, "?GetIsSelectable@GoAspect@@QBE_NXZ");
    O(GoAspect_GetIsVisible, "?GetIsVisible@GoAspect@@QBE_NXZ");
    O(Go_GetFollower, "?GetFollower@Go@@QAEPAVGoFollower@@XZ");
    O(GoPlacement_GetOrientation, "?GetOrientation@GoPlacement@@QBEABUQuat@@XZ");
#undef O
    g_directOK = Go_GetBody && GoBody_SAnimate && MakeAnimReq && GoBody_GetAvgMoveVelocity && GoBody_GetPerm &&
                 AIQuery_IsAreaWalkable && GoPlacement_SSetPosition && GoPlacement_OrientToPosition &&
                 Aspect_GetCurrentChore && Aspect_GetNextChore && Aspect_GetCurrentVelocity && GoMind_ClearQ &&
                 Go_GetAspect && GoAspect_GetAspectPtr && Go_GetFollower && GoPlacement_GetOrientation &&
                 g_plApproach >= 0;
    logf_("direct control link: %d", g_directOK);
    logf_("movement planner link: pl_approach=%d, tint=%d", g_plApproach, Go_GetAspect && GoAspect_GetAspectPtr);

    g_missing = 0;
    S(WorldState_Get, "?FUBI_GetClassSingleton@WorldState@@CAPAV1@XZ");
    S(WorldState_GetCurrentState, "?GetCurrentState@WorldState@@QBE?AW4eWorldState@@XZ");
    S(WS_ToString, "?ToString@@YAPBDW4eWorldState@@@Z");
    S(UIShell_Get, "?FUBI_GetClassSingleton@UIShell@@CAPAV1@XZ");
    S(AppModule_Get, "?FUBI_GetClassSingleton@AppModule@@CAPAV1@XZ");
    AppModule_IsUserPaused = (void *)GetProcAddress(g_exe, "?IsUserPaused@AppModule@@QAE_NXZ");
    S(AppModule_GetCursorX, "?GetCursorX@AppModule@@QBEHXZ");
    S(AppModule_GetCursorY, "?GetCursorY@AppModule@@QBEHXZ");
    S(UIType_ToString, "?ToString@@YAPBDW4UI_CONTROL_TYPE@@@Z");
    S(UIShell_GetScreenWidth, "?GetScreenWidth@UIShell@@QBEHXZ");
    S(UIShell_GetScreenHeight, "?GetScreenHeight@UIShell@@QBEHXZ");
    S(UIWindow_GetType, "?GetType@UIWindow@@QBE?AW4UI_CONTROL_TYPE@@XZ");
    S(UIWindow_GetRect, "?GetRect@UIWindow@@QAEAAUGRect@@XZ");
    S(UIWindow_GetVisible, "?GetVisible@UIWindow@@QBE_NXZ");
    S(UIWindow_IsEnabled, "?IsEnabled@UIWindow@@QBE_NXZ");
    S(UIWindow_GetChildren, "?GetChildren@UIWindow@@QAEAAUUIWindowVec@@XZ");
    S(UIWindowVec_Size, "?Size@UIWindowVec@@ABEHXZ");
    S(UIWindowVec_Get, "?Get@UIWindowVec@@ABEPAVUIWindow@@H@Z");
    S(UIGridbox_GetBoxWidth, "?GetBoxWidth@UIGridbox@@QBEHXZ");
    S(UIGridbox_GetBoxHeight, "?GetBoxHeight@UIGridbox@@QBEHXZ");
    S(UIGridbox_GetColumns, "?GetColumns@UIGridbox@@QBEHXZ");
    S(UIGridbox_GetRows, "?GetRows@UIGridbox@@QBEHXZ");
    g_uiOK = (g_missing == 0);
#undef S

    /* Non-exported pieces, valid only for the exact 1.50 build: verify code bytes first. */
    static const unsigned char adjSig[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x68, 0x53, 0x56, 0x8b, 0x75, 0x08 };
    static const unsigned char lbSig[] = { 0x3b, 0x35, 0x5c, 0xd8, 0x7a, 0x00 }; /* cmp esi,[0x7ad85c] */
    if (rd((void *)0x66ebe4, sizeof adjSig) && !memcmp((void *)0x66ebe4, adjSig, sizeof adjSig))
        Engine_AdjustPointToTerrain = (void *)0x66ebe4;
    else
        logf_("terrain snap routine not found at expected address; moves will be sent unsnapped");
    if (rd((void *)0x6f0b04, sizeof lbSig) && !memcmp((void *)0x6f0b04, lbSig, sizeof lbSig))
        g_mapNil = (void **)0x7ad85c;
    else {
        logf_("UI map sentinel not found at expected address; menu navigation disabled");
        g_uiOK = 0;
    }
    logf_("engine link: gameplay=%d ui=%d terrainSnap=%d", g_gameOK, g_uiOK, Engine_AdjustPointToTerrain != NULL);
}

/* ------------------------------------------------------------------ config */
static float c_deadzone = 0.24f, c_moveDist = 6.0f, c_attackRadius = 9.0f, c_interactRadius = 6.0f;
static float c_followDist = 3.5f, c_cursorSpeed = 900.0f;
static int c_moveIntervalMs = 180, c_stickXSign = 0, c_follow = 1, c_navRepeatMs = 170, c_swapSticks = 0;
static int c_pad = -1, c_highlight = 1, c_moveMode = 2;
static float c_speedScale = 1.0f, c_bodyRadius = 0.3f, c_actorSpacing = 1.4f, c_strafeScale = 0.6f;
static int c_strafe = 2, c_brainOff = 1, c_kiteWalkMs = 350, c_shadowPlan = 1, c_shadowMs = 250;
static float c_uiScale = 1.5f;
static int c_uiScaleMinWidth = 1280;
static char c_uiNoScale[512];
static int c_uiScaleLabels = 0;
static int g_walkCheck = 1;
static float c_flushDelay = 0.12f;
static char c_hud[1024];
static int k_inventory, k_map, k_pause, k_menu, k_weaponCycle, k_loot, k_spellbook, k_journal, k_selectAll;
static int k_camLeft, k_camRight, k_camUp, k_camDown, k_zoomIn, k_zoomOut, k_awp[8];

static float cfg_f(const char *key, float def)
{
    char buf[64], d[64];
    snprintf(d, sizeof d, "%g", def);
    GetPrivateProfileStringA("DSPad", key, d, buf, sizeof buf, g_ini);
    return (float)atof(buf);
}
static int c_autoPotion = 1;
static float c_autoPotionPct = 30.0f;
static int cfg_i(const char *key, int def) { return (int)GetPrivateProfileIntA("DSPad", key, def, g_ini); }

static int parse_key(const char *s)
{
    static const struct { const char *n; int vk; } tab[] = {
        { "ESC", VK_ESCAPE }, { "ESCAPE", VK_ESCAPE }, { "TAB", VK_TAB }, { "SPACE", VK_SPACE },
        { "ENTER", VK_RETURN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "UP", VK_UP }, { "DOWN", VK_DOWN },
        { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD }, { "MINUS", VK_OEM_MINUS }, { "EQUALS", VK_OEM_PLUS },
        { "SLASH", VK_OEM_2 }, { "LBRACKET", VK_OEM_4 }, { "RBRACKET", VK_OEM_6 }, { "PAGEUP", VK_PRIOR },
        { "PAGEDOWN", VK_NEXT }, { "HOME", VK_HOME }, { "END", VK_END }, { "NONE", 0 }
    };
    if (!s || !*s) return 0;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!lstrcmpiA(s, tab[i].n)) return tab[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') return VK_F1 + atoi(s + 1) - 1;
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    return 0;
}
static int cfg_key(const char *key, const char *def)
{
    char buf[32];
    GetPrivateProfileStringA("Keys", key, def, buf, sizeof buf, g_ini);
    return parse_key(buf);
}

static int c_camLock, c_miniMap, c_mmSize, c_mmMargin, c_itemInfo, c_infoX, c_infoY, c_hideCursor, c_compare, c_targetBar, c_crossHelp, c_liftZone;
static float c_mmMeters;
static void load_config(void)
{
    c_camLock = cfg_i("CameraLockToLeader", 1);
    c_miniMap = cfg_i("MiniMap", 1);
    c_itemInfo = cfg_i("GroundItemInfo", 1);
    c_hideCursor = cfg_i("HideCursorWithPad", 1);
    c_compare = cfg_i("CompareWithEquipped", 1);
    c_targetBar = cfg_i("TargetInfoBar", 1);
    c_crossHelp = cfg_i("PathfinderCrossingHelp", 0);
    c_liftZone = cfg_i("LiftZonePathfinder", 0);
    c_infoX = cfg_i("GroundItemInfoX", 60);
    c_infoY = cfg_i("GroundItemInfoY", 45);
    c_mmSize = cfg_i("MiniMapSize", 200);
    c_mmMargin = cfg_i("MiniMapMargin", 10);
    c_mmMeters = cfg_f("MiniMapMeters", 60.0f);
    g_logLevel = cfg_i("Log", 1);
    c_deadzone = cfg_f("Deadzone", 0.24f);
    c_moveDist = cfg_f("MoveDistance", 6.0f);
    c_moveIntervalMs = cfg_i("MoveIntervalMs", 120);
    c_attackRadius = cfg_f("AttackRadius", 9.0f);
    c_interactRadius = cfg_f("InteractRadius", 6.0f);
    c_follow = cfg_i("PartyFollow", 1);
    c_followDist = cfg_f("FollowDistance", 3.5f);
    c_autoPotion = cfg_i("PartyAutoPotion", 1);
    c_autoPotionPct = cfg_f("PartyAutoPotionPercent", 30.0f);
    c_stickXSign = cfg_i("StickXSign", 0);
    c_cursorSpeed = cfg_f("CursorSpeed", 900.0f);
    c_navRepeatMs = cfg_i("NavRepeatMs", 170);
    c_swapSticks = cfg_i("SwapSticks", 0);
    c_pad = cfg_i("Controller", -1);
    c_highlight = cfg_i("HighlightTarget", 1);
    c_moveMode = cfg_i("MoveMode", 2);
    c_speedScale = cfg_f("WalkSpeedScale", 1.0f);
    c_bodyRadius = cfg_f("BodyRadius", 0.3f);
    g_walkCheck = cfg_i("WalkCheck", 1);
    c_actorSpacing = cfg_f("EnemySpacing", 1.4f);
    c_strafe = cfg_i("MoveWhileAttacking", 2);
    c_kiteWalkMs = cfg_i("StepMsBetweenAttacks", 350);
    c_brainOff = cfg_i("PauseHeroAIWhileWalking", 1);
    c_shadowPlan = cfg_i("TriggerShadowPath", 1);
    c_shadowMs = cfg_i("TriggerShadowMs", 250);
    c_uiScale = cfg_f("UIScale", 1.5f);
    c_uiScaleMinWidth = cfg_i("UIScaleMinWidth", 1280);
    c_uiScaleLabels = cfg_i("UIScaleItemLabels", 0);
    GetPrivateProfileStringA("DSPad", "UIScaleExclude", "", c_uiNoScale + 1, sizeof c_uiNoScale - 3, g_ini);
    c_uiNoScale[0] = ',';
    lstrcatA(c_uiNoScale, ",");
    c_strafeScale = cfg_f("HoverSpeed", 0.6f);
    c_flushDelay = cfg_f("ReplanDelay", 0.12f);
    GetPrivateProfileStringA("DSPad", "HudInterfaces",
        "compass_hotpoints,console_output,data_bar,field_commands,game_console,game_console_mp,icons,"
        "member_labels,mini_map,status_bars,team_portraits,rollover_help,world_tip,nis_subtitle,"
        "character_awp,debug,cursors,common,tool_tip,fade_screen,jip_glass,load_bar,multiplayer_timer,"
        "multiplayer_ranks,pause,quick_save",
        c_hud + 1, sizeof c_hud - 3, g_ini);
    c_hud[0] = ',';
    lstrcatA(c_hud, ",");
    k_inventory = cfg_key("Inventory", "I");
    k_map = cfg_key("MegaMap", "TAB");
    k_pause = cfg_key("Pause", "SPACE");
    k_menu = cfg_key("Menu", "ESC");
    k_weaponCycle = cfg_key("QuickWeaponSelect", "Q");
    k_loot = cfg_key("CollectLoot", "Z");
    k_spellbook = cfg_key("SpellBook", "B");
    k_journal = cfg_key("Journal", "J");
    k_selectAll = cfg_key("SelectAll", "E");
    k_camLeft = cfg_key("CameraLeft", "LEFT");
    k_camRight = cfg_key("CameraRight", "RIGHT");
    k_camUp = cfg_key("CameraUp", "UP");
    k_camDown = cfg_key("CameraDown", "DOWN");
    k_zoomIn = cfg_key("ZoomIn", "EQUALS");
    k_zoomOut = cfg_key("ZoomOut", "MINUS");
    for (int i = 0; i < 8; i++) {
        char n[16], d[4];
        snprintf(n, sizeof n, "WeaponConfig%d", i + 1);
        snprintf(d, sizeof d, "%d", i + 1);
        k_awp[i] = cfg_key(n, d);
    }
}

/* ------------------------------------------------------------------ synthesized input */
static bool8 g_keyHeld[256];
static int g_mouseL, g_mouseR;
static HWND g_hwnd;

static void send_key(int vk, int down)
{
    if (vk <= 0 || vk > 255) return;
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, 0);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
        vk == VK_HOME || vk == VK_END)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof in);
}
static void hold_key(int vk, int down)
{
    if (vk <= 0 || vk > 255) return;
    if ((g_keyHeld[vk] != 0) == (down != 0)) return;
    g_keyHeld[vk] = (bool8)(down != 0);
    send_key(vk, down);
}
static void tap_key(int vk)
{
    if (vk <= 0 || vk > 255 || g_keyHeld[vk]) return;
    send_key(vk, 1);
    send_key(vk, 0);
}
static void mouse_button(int right, int down)
{
    int *st = right ? &g_mouseR : &g_mouseL;
    if (*st == (down != 0)) return;
    *st = (down != 0);
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = right ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
                          : (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    SendInput(1, &in, sizeof in);
}
static void release_all(void)
{
    for (int i = 1; i < 256; i++)
        if (g_keyHeld[i]) hold_key(i, 0);
    mouse_button(0, 0);
    mouse_button(1, 0);
}

/* ------------------------------------------------------------------ UI enumeration */
enum { UK_NONE = 0, UK_POINT, UK_GRID, UK_LIST };
static signed char g_typeKind[64];

static int type_kind(int t)
{
    if (t < 0 || t >= 64) return UK_NONE;
    if (g_typeKind[t] >= 0) return g_typeKind[t];
    int k = UK_NONE;
    const char *n = UIType_ToString(t);
    if (n && !IsBadStringPtrA(n, 64)) {
        if (strstr(n, "gridbox")) k = UK_GRID;
        else if (strstr(n, "listbox") || strstr(n, "listreport")) k = UK_LIST;
        else if (strstr(n, "itemslot") || strstr(n, "infoslot") || strstr(n, "button") || strstr(n, "checkbox") ||
                 strstr(n, "slider") || strstr(n, "combo_box") || strstr(n, "edit_box") || strstr(n, "popupmenu"))
            k = UK_POINT;
        else {
            size_t l = strlen(n);
            if (l >= 3 && !strcmp(n + l - 3, "tab")) k = UK_POINT;
        }
    }
    g_typeKind[t] = (signed char)k;
    return k;
}

typedef struct { short x, y; const char *type; GRect rc; } NavPt;
#define MAX_PTS 1500
static NavPt g_pts[MAX_PTS];
static int g_npts;
static int g_uiW = 640, g_uiH = 480;

static const char *g_addType = "";
static GRect g_addRect;
static void add_pt(int x, int y)
{
    if (x < 1 || y < 1 || x >= g_uiW - 1 || y >= g_uiH - 1 || g_npts >= MAX_PTS) return;
    for (int i = g_npts - 1; i >= 0 && i > g_npts - 400; i--)
        if (abs(g_pts[i].x - x) < 6 && abs(g_pts[i].y - y) < 6) return;
    g_pts[g_npts].x = (short)x;
    g_pts[g_npts].y = (short)y;
    g_pts[g_npts].type = g_addType;
    g_pts[g_npts].rc = g_addRect;
    g_npts++;
}

static int g_winBudget;
static void add_window(void *w, int depth)
{
    if (depth > 6 || --g_winBudget < 0 || !rd(w, 0x1c8)) return;
    if (!UIWindow_GetVisible(w)) return;
    GRect *r = UIWindow_GetRect(w);
    int W = r->r - r->l, H = r->b - r->t;
    if (UIWindow_IsEnabled(w) && W > 3 && H > 3 && W <= g_uiW + 8 && H <= g_uiH + 8) {
        int ty = UIWindow_GetType(w);
        int kind = type_kind(ty);
        if (kind != UK_NONE) { g_addType = UIType_ToString(ty); g_addRect = *r; }
        if (kind == UK_POINT) {
            add_pt((r->l + r->r) / 2, (r->t + r->b) / 2);
        } else if (kind == UK_GRID) {
            int bw = UIGridbox_GetBoxWidth(w), bh = UIGridbox_GetBoxHeight(w);
            int cols = UIGridbox_GetColumns(w), rows = UIGridbox_GetRows(w);
            if (bw < 8 || bw > 256) bw = 32;
            if (bh < 8 || bh > 256) bh = 32;
            if (cols < 1 || cols > 64) cols = W / bw;
            if (rows < 1 || rows > 64) rows = H / bh;
            for (int j = 0; j < rows && j < 64; j++)
                for (int i = 0; i < cols && i < 64; i++)
                    add_pt(r->l + i * bw + bw / 2, r->t + j * bh + bh / 2);
        } else if (kind == UK_LIST) {
            int step = g_uiH >= 700 ? 22 : 18;
            for (int y = r->t + step / 2; y < r->b; y += step)
                add_pt((r->l + r->r) / 2, y);
        }
    }
    void *vec = UIWindow_GetChildren(w);
    int n = UIWindowVec_Size(vec);
    if (n > 0 && n < 512)
        for (int i = 0; i < n; i++)
            add_window(UIWindowVec_Get(vec, i), depth + 1);
}

static int is_hud(const char *name)
{
    char key[80];
    if (!strncmp(name, "chapter_", 8)) return 1;
    snprintf(key, sizeof key, ",%s,", name);
    return strstr(c_hud, key) != NULL;
}

/* Collect interactive points. menuOnly=1 skips HUD interfaces. Returns number of non-HUD
   interfaces that are visible and contain something to press. */
static char g_menuNames[256];
static int collect_ui(int includeHud)
{
    g_npts = 0;
    g_menuNames[0] = 0;
    if (!g_uiOK) return 0;
    char *shell = UIShell_Get();
    if (!rd(shell, 0x90)) return 0;
    g_uiW = UIShell_GetScreenWidth(shell);
    g_uiH = UIShell_GetScreenHeight(shell);
    if (g_uiW < 320 || g_uiW > 16384 || g_uiH < 200 || g_uiH > 16384) return 0;
    char **b = *(char ***)(shell + 4), **e = *(char ***)(shell + 8);
    int n = (int)(e - b);
    if (n <= 0 || n > 1024 || !rd(b, n * sizeof(void *))) return 0;
    void *nil = *g_mapNil;
    int menus = 0;
    g_winBudget = 6000;
    for (int i = 0; i < n; i++) {
        char *iface = b[i];
        if (!rd(iface, 0x34) || !iface[2]) continue;
        const char *name = *(const char **)(iface + 4);
        if (!name || IsBadStringPtrA(name, 64)) continue;
        int hud = is_hud(name);
        if (hud && !includeHud) continue;
        int before = g_npts;
        /* in-order walk of the VC6 red-black tree at iface+0x24 */
        void **head = *(void ***)(iface + 0x28);
        if (!rd(head, 0x18)) continue;
        void **stack[64];
        int sp = 0, guard = 0;
        void **node = (void **)head[1];
#define TREE_OK(p) ((p) && (void *)(p) != nil && (p) != head && rd((p), 0x18))
        for (;;) {
            while (TREE_OK(node) && sp < 64) {
                stack[sp++] = node;
                node = (void **)node[0];
            }
            if (sp == 0 || ++guard > 4000) break;
            node = stack[--sp];
            add_window(node[5], 0);
            node = (void **)node[2];
        }
#undef TREE_OK
        if (!hud && g_npts > before) {
            menus++;
            if (strlen(g_menuNames) + strlen(name) + 2 < sizeof g_menuNames) {
                lstrcatA(g_menuNames, name);
                lstrcatA(g_menuNames, " ");
            }
        }
    }
    return menus;
}

/* ------------------------------------------------------------------ UI scaler
   The UI is laid out for a smaller "virtual" screen (real size / scale) and every 2D draw made
   while a UI window is drawing is enlarged by the same factor at the Direct3D call. The 3D scene,
   and anything drawn outside the UI window pass, is untouched. */
static float g_uiS = 1.0f;   /* scale currently in effect (1 = off) */
static int g_inUiDraw;       /* a scaled UI window is drawing right now */
int dspad_ui_force;          /* game code that draws HUD pieces itself (compass, item labels) is running */
static int g_realW, g_realH;

typedef HRESULT (WINAPI *DrawPrim_t)(void *, DWORD, DWORD, void *, DWORD, DWORD);
typedef HRESULT (WINAPI *DrawIdx_t)(void *, DWORD, DWORD, void *, DWORD, WORD *, DWORD, DWORD);
/* The menu and the game use different Direct3D device classes, each with its own function table,
   so the original functions are remembered per table. */
static struct { void **vt; DrawPrim_t dp; DrawIdx_t dip; } g_devs[6];
static int g_ndevs;
static int dev_slot(void *dev)
{
    void **vt = *(void ***)dev;
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].vt == vt) return i;
    return 0;
}
static BYTE g_vbuf[0x40000];

static unsigned fvf_stride(DWORD fvf)
{
    unsigned n = 16; /* XYZRHW */
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    n += ((fvf >> 8) & 0xf) * 8;
    return n;
}
static void *scale_verts(DWORD fvf, void *v, DWORD count)
{
    unsigned stride = fvf_stride(fvf);
    if (!count || count * stride > sizeof g_vbuf || IsBadReadPtr(v, count * stride)) return v;
    memcpy(g_vbuf, v, count * stride);
    for (DWORD i = 0; i < count; i++) {
        float *p = (float *)(g_vbuf + i * stride);
        p[0] = (p[0] + 0.5f) * g_uiS - 0.5f;
        p[1] = (p[1] + 0.5f) * g_uiS - 0.5f;
    }
    return g_vbuf;
}
static HRESULT WINAPI hk_DrawPrimitive(void *dev, DWORD type, DWORD fvf, void *v, DWORD count, DWORD flags)
{
    if ((g_inUiDraw || dspad_ui_force) && g_uiS != 1.0f && (fvf & 0xe) == 0x4) v = scale_verts(fvf, v, count);
    return g_devs[dev_slot(dev)].dp(dev, type, fvf, v, count, flags);
}
static HRESULT WINAPI hk_DrawIndexedPrimitive(void *dev, DWORD type, DWORD fvf, void *v, DWORD vcount, WORD *idx,
                                              DWORD icount, DWORD flags)
{
    if ((g_inUiDraw || dspad_ui_force) && g_uiS != 1.0f && (fvf & 0xe) == 0x4) v = scale_verts(fvf, v, vcount);
    return g_devs[dev_slot(dev)].dip(dev, type, fvf, v, vcount, idx, icount, flags);
}
/* The renderer's Direct3D device can be recreated (menu -> game), so this is re-checked every tick. */
static void ensure_d3d_hook(void)
{
    char *shell = UIShell_Get ? UIShell_Get() : NULL;
    if (!rd(shell, 0x90)) return;
    char *rapi = *(char **)(shell + 0x68);
    if (!rd(rapi, 0x604)) return;
    void ***dev = *(void ****)(rapi + 0x600);
    if (!rd(dev, 4)) return;
    void **vt = *dev;
    if (!rd(vt, 0x80) || (vt[25] == (void *)hk_DrawPrimitive && vt[26] == (void *)hk_DrawIndexedPrimitive)) return;
    if (vt[25] == (void *)hk_DrawPrimitive || vt[26] == (void *)hk_DrawIndexedPrimitive) return; /* half-patched: leave alone */
    int slot = g_ndevs;
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].vt == vt) slot = i; /* same table again: the Direct3D module was reloaded */
    if (slot >= 6) return;
    DWORD old;
    if (!VirtualProtect(&vt[25], 8, PAGE_READWRITE, &old)) {
        static int warned;
        if (!warned++) logf_("ui scale: cannot patch the Direct3D device table");
        return;
    }
    g_devs[slot].vt = vt;
    g_devs[slot].dp = (DrawPrim_t)vt[25];
    g_devs[slot].dip = (DrawIdx_t)vt[26];
    if (slot == g_ndevs) g_ndevs++;
    vt[25] = (void *)hk_DrawPrimitive;
    vt[26] = (void *)hk_DrawIndexedPrimitive;
    VirtualProtect(&vt[25], 8, old, &old);
    logf_("ui scale: Direct3D draw hook installed (device table %p, #%d)", (void *)vt, g_ndevs);
}

/* Is this window part of an interface that should stay unscaled (things pinned to the 3D world)? */
static int window_scaled(char *win)
{
    /* Item name labels on the ground are UI windows placed at real screen positions taken from
       the 3D world, so they stay unscaled. */
    const char *wn = *(const char **)(win + 0x9c);
    if (wn && !c_uiScaleLabels && !strncmp(wn, "item_overlay_text", 17)) return 0;
    const char *name = *(const char **)(win + 0x80);
    if (!name) return 1;
    for (const char *p = c_uiNoScale; *p; p++) {
        if (*p != ',') continue;
        const char *a = p + 1, *b = name;
        while (*a && *a != ',' && *a == *b) { a++; b++; }
        if (*a == ',' && *b == 0 && a != p + 1) return 0;
    }
    return 1;
}
static int g_hideCursor, g_shutdown, g_padLast, g_padPlay;
static DWORD g_lastResize;
static void TC hk_window_draw(char *win)
{
    int prev = g_inUiDraw;
    g_inUiDraw = g_uiS != 1.0f && window_scaled(win);
    (*(void (TC **)(void *))(*(char **)win + 0x40))(win);
    g_inUiDraw = prev;
}
static void ui_virtual_size(int *w, int *h)
{
    g_realW = *w;
    g_realH = *h;
    float sc = (c_uiScale > 1.01f && c_uiScale < 4.0f && *w >= c_uiScaleMinWidth) ? c_uiScale : 1.0f;
    if (sc != g_uiS) logf_("ui scale: %dx%d -> factor %.2f", *w, *h, sc);
    g_uiS = sc;
    if (sc != 1.0f) {
        *w = (int)(*w / sc + 0.5f);
        *h = (int)(*h / sc + 0.5f);
    }
}
static void TC hk_ui_resize(void *shell, int w, int h)
{
    g_lastResize = timeGetTime();
    ui_virtual_size(&w, &h);
    ((void (TC *)(void *, int, int))0x6ea4ff)(shell, w, h);
}
static void TC hk_ui_setsize(void *shell, int w, int h)
{
    ui_virtual_size(&w, &h);
    ((void (TC *)(void *, int, int))0x6e8132)(shell, w, h);
}
static bool8 TC hk_ui_mouse(void *shell, int x, int y)
{
    if (g_uiS != 1.0f) {
        x = (int)(x / g_uiS);
        y = (int)(y / g_uiS);
    }
    return ((bool8 (TC *)(void *, int, int))0x6eaf45)(shell, x, y);
}
/* The character model in the inventory is a 3D render whose placement comes from a per-resolution
   table. With the UI scaled, the entry for the virtual resolution is the one that lines up. */
static void TC hk_paperdoll_select(char *self)
{
    /* decided from the game's own resolution, because this can run before the UI is resized */
    char *app = AppModule_Get ? AppModule_Get() : NULL;
    int rw = rd(app, 0xd0) ? *(int *)(app + 0xc8) - *(int *)(app + 0xc0) : 0;
    int rh = rd(app, 0xd0) ? *(int *)(app + 0xcc) - *(int *)(app + 0xc4) : 0;
    if (c_uiScale > 1.01f && c_uiScale < 4.0f && rw >= c_uiScaleMinWidth && rh > 0) {
        float vw = (float)(int)(rw / c_uiScale + 0.5f), vh = (float)(int)(rh / c_uiScale + 0.5f);
        float *e = *(float **)(self + 0x9c), *end = *(float **)(self + 0xa0);
        for (int n = 0; e != end && n < 64 && rd(e, 0x20); e += 8, n++) {
            if (e[5] != vw || e[6] != vh) continue;
            *(float *)(self + 0xa8) = e[0];
            *(float *)(self + 0xac) = e[1];
            *(float *)(self + 0xb0) = e[3];
            *(float *)(self + 0xb4) = e[4];
            *(float *)(self + 0xb8) = e[2];
            *(float *)(self + 0xbc) = e[7];
            return;
        }
        static int warned;
        if (!warned++) logf_("ui scale: no character-model placement entry for %gx%g; using the real resolution's", vw, vh);
    }
    ((void (TC *)(char *))0x518990)(self);
}

/* Two HUD pieces are drawn by game code rather than by UI windows, but are positioned in the UI's
   (now virtual) coordinates: the compass and the item labels on the ground. Their draw routines are
   wrapped so the scale applies while they run. The wrappers work for any argument count. */
void *dspad_orig_compass = (void *)0x6759aa, *dspad_orig_labels = (void *)0x4edc07;
void *dspad_ret_compass, *dspad_ret_labels;
__asm__(".text\n"
        ".globl _dspad_wrap_compass\n"
        "_dspad_wrap_compass:\n"
        "  popl _dspad_ret_compass\n"
        "  incl _dspad_ui_force\n"
        "  call *_dspad_orig_compass\n"
        "  decl _dspad_ui_force\n"
        "  pushl _dspad_ret_compass\n"
        "  ret\n"
        ".globl _dspad_wrap_labels\n"
        "_dspad_wrap_labels:\n"
        "  popl _dspad_ret_labels\n"
        "  incl _dspad_ui_force\n"
        "  call *_dspad_orig_labels\n"
        "  decl _dspad_ui_force\n"
        "  pushl _dspad_ret_labels\n"
        "  ret\n");
extern void dspad_wrap_compass(void);
extern void dspad_wrap_labels(void);

static int patch_bytes(DWORD site, const BYTE *expect, int n, void *callTarget)
{
    BYTE *p = (BYTE *)site;
    DWORD old;
    if (!rd(p, n) || memcmp(p, expect, n)) return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xe8;
    *(int *)(p + 1) = (int)((DWORD)callTarget - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}
static int patch_call_rel(DWORD site, DWORD oldTarget, void *newTarget)
{
    BYTE *p = (BYTE *)site;
    DWORD old;
    if (!rd(p, 5) || (p[0] != 0xe8 && p[0] != 0xe9) || (DWORD)(site + 5 + *(int *)(p + 1)) != oldTarget) return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int *)(p + 1) = (int)((DWORD)newTarget - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}
static int g_uiScaleHooked;
static void install_ui_scale(void)
{
    static const BYTE vcall[] = { 0x8b, 0x01, 0xff, 0x50, 0x40 }; /* mov eax,[ecx] ; call [eax+0x40] */
    static const DWORD drawSites[] = { 0x6ea26f, 0x6ea3cd, 0x6ea43b, 0x6ea49a };
    if (!(c_uiScale > 1.01f) || !g_uiOK) return;
    /* check everything first so the patch set is all-or-nothing */
    for (int i = 0; i < 4; i++)
        if (!rd((void *)drawSites[i], 5) || memcmp((void *)drawSites[i], vcall, 5)) { logf_("ui scale: draw site %d not as expected; disabled", i); return; }
    static const DWORD callSites[][2] = { { 0x476e4a, 0x6ea4ff }, { 0x4c539f, 0x6ea4ff }, { 0x6e7e42, 0x6e8132 }, { 0x6e9f7c, 0x6eaf45 } };
    for (int i = 0; i < 4; i++) {
        BYTE *p = (BYTE *)callSites[i][0];
        if (!rd(p, 5) || p[0] != 0xe8 || (DWORD)(callSites[i][0] + 5 + *(int *)(p + 1)) != callSites[i][1]) {
            logf_("ui scale: call site %d not as expected; disabled", i);
            return;
        }
    }
    int ok = 1;
    for (int i = 0; i < 4; i++) ok &= patch_bytes(drawSites[i], vcall, 5, hk_window_draw);
    ok &= patch_call_rel(0x476e4a, 0x6ea4ff, hk_ui_resize);
    ok &= patch_call_rel(0x4c539f, 0x6ea4ff, hk_ui_resize);
    ok &= patch_call_rel(0x6e7e42, 0x6e8132, hk_ui_setsize);
    ok &= patch_call_rel(0x6e9f7c, 0x6eaf45, hk_ui_mouse);
    {
        int d1 = patch_call_rel(0x4a8d1e, 0x518990, hk_paperdoll_select), d2 = patch_call_rel(0x51897a, 0x518990, hk_paperdoll_select);
        logf_("ui scale: character-model placement hooks %d/%d", d1, d2);
        int c1 = patch_call_rel(0x4776b4, 0x6759aa, dspad_wrap_compass);
        logf_("ui scale: compass hook %d", c1);
    }
    g_uiScaleHooked = ok;
    logf_("ui scale: hooks %s, factor %.2f for widths >= %d", ok ? "installed" : "FAILED", c_uiScale, c_uiScaleMinWidth);
}

/* ------------------------------------------------------------------ mini map
   The game's full-screen map (TAB) is drawn by one engine routine that takes a pixel size. Each frame,
   after the 3D world and before the HUD, the same routine is run again into a small square in the
   top-right corner, and the compass that normally sits there is skipped. */
typedef struct { DWORD x, y, w, h; float minz, maxz; } D3DVP7;
static void *get_leader(void);
static int g_mmDrawn;
static DWORD g_mmLists[10]; /* the map routine's work lists, kept apart from the main view's */
static void *g_compassPrev = (void *)0x6759aa;
static void minimap_draw(void)
{
    static int logged;
    g_mmDrawn = 0;
    if (!c_miniMap || !g_uiOK || g_shutdown) return;
    if (timeGetTime() - g_lastResize < 1500) return; /* display is being switched (leaving or entering a game) */
    if (!get_leader()) return;
    char *ws = *(char **)0x7acf44;
    if (!rd(ws, 0x2c)) return;
    int st = *(int *)(ws + 0x20), pend = *(int *)(ws + 0x28);
    if (pend && pend != st) return;
    const char *sn = WS_ToString ? WS_ToString(st) : NULL;
    if (!sn || IsBadStringPtrA(sn, 48) || !strstr(sn, "ingame") || strstr(sn, "menu") || strstr(sn, "jip")) return;
    char *eng = SiegeEngine_Get();
    if (!rd(eng, 0xc4)) return;
    char *obj = *(char **)(eng + 0x38), *compass = *(char **)(eng + 0x40), *rapi = *(char **)(eng + 0x44);
    if (!rd(obj, 0xa4) || !rd(compass, 0x50) || !rd(rapi, 0xd48)) return;
    if (!compass[0x4c] || !compass[0x4d]) return; /* compass hidden: map hidden too */
    void **dev = *(void ***)(rapi + 0x600);
    if (!rd(dev, 4) || !rd(*dev, 0x70)) return;
    void **vt = *(void ***)dev;
    int W = *(int *)(rapi + 0x674), H = *(int *)(rapi + 0x678);
    float sc = g_uiS > 1.0f ? g_uiS : 1.0f;
    int size = (int)(c_mmSize * sc), margin = (int)(c_mmMargin * sc);
    if (W < 320 || H < 240 || W > 16384 || H > 16384) return;
    if (size > (W < H ? W : H) / 2) size = (W < H ? W : H) / 2;
    if (size < 32) return;
    int x = W - margin - size, y = margin;
    D3DVP7 old, vp;
    if (((HRESULT (WINAPI *)(void *, D3DVP7 *))vt[15])(dev, &old) != 0) return;
    float mView[16], mProj[16], mProj2[16];
    memcpy(mView, rapi + 0xc88, 64);
    memcpy(mProj, rapi + 0xcc8, 64);
    memcpy(mProj2, rapi + 0xd08, 64);
    float oSize = *(float *)(obj + 0x5c), oMax = *(float *)(obj + 0x60), oMin = *(float *)(obj + 0x64);
    char z1 = obj[0x68], z2 = obj[0x69];
    DWORD tmp[10];
    memcpy(tmp, obj + 4, 40);
    memcpy(obj + 4, g_mmLists, 40);
    /* frame */
    {
        LONG rc[4] = { x - 2, y - 2, x + size + 2, y + size + 2 };
        ((HRESULT (WINAPI *)(void *, DWORD, void *, DWORD, DWORD, float, DWORD))vt[10])(dev, 1, rc, 1, 0xff5a4a32, 1.0f, 0);
    }
    vp.x = x; vp.y = y; vp.w = size; vp.h = size; vp.minz = 0.0f; vp.maxz = 1.0f;
    ((HRESULT (WINAPI *)(void *, D3DVP7 *))vt[13])(dev, &vp);
    *(int *)(rapi + 0x674) = size;
    *(int *)(rapi + 0x678) = size;
    /* the game's own zoom limits stay in place: the marker size is derived from them */
    {
        float m = c_mmMeters;
        if (oMin > 0.0f && m < oMin) m = oMin;
        if (oMax > oMin && m > oMax) m = oMax;
        *(float *)(obj + 0x5c) = m;
    }
    obj[0x68] = obj[0x69] = 0;
    /* the map routine also does mouse picking; park the cursor farPt away so nothing on the mini map is
       ever "under the mouse", and keep the main view's pick result */
    char *ms = *(char **)(eng + 0x48);
    float msPos[3], msDrag[3];
    DWORD msHit[6];
    int msOK = rd(ms, 0x1b0) != 0;
    if (msOK) {
        memcpy(msPos, ms + 0x28, 12);
        memcpy(msDrag, ms + 0xc, 12);
        memcpy(msHit, ms + 0x194, 24);
        ((float *)(ms + 0x28))[0] = ((float *)(ms + 0x28))[1] = 1.0e7f;
        ((float *)(ms + 0xc))[0] = ((float *)(ms + 0xc))[1] = 1.0e7f;
    }
    ((void (TC *)(void *, int, int, int, int))0x6775e7)(obj, 0, size, 0, size);
    *(float *)(obj + 0x5c) = oSize;
    if (msOK) {
        memcpy(ms + 0x28, msPos, 12);
        memcpy(ms + 0xc, msDrag, 12);
        memcpy(ms + 0x194, msHit, 24);
    }
    obj[0x68] = z1; obj[0x69] = z2;
    memcpy(g_mmLists, obj + 4, 40);
    memcpy(obj + 4, tmp, 40);
    *(int *)(rapi + 0x674) = W;
    *(int *)(rapi + 0x678) = H;
    memcpy(rapi + 0xc88, mView, 64);
    memcpy(rapi + 0xcc8, mProj, 64);
    memcpy(rapi + 0xd08, mProj2, 64);
    ((HRESULT (WINAPI *)(void *, D3DVP7 *))vt[13])(dev, &old);
    ((HRESULT (WINAPI *)(void *, DWORD, void *))vt[11])(dev, 2, rapi + 0xc88);
    ((HRESULT (WINAPI *)(void *, DWORD, void *))vt[11])(dev, 3, rapi + 0xcc8);
    g_mmDrawn = 1;
    if (!logged) { logged = 1; logf_("mini map: drawing %dx%d at (%d,%d), %.0f m across (screen %dx%d, game map size %.0f, limits %.0f-%.0f)", size, size, x, y, c_mmMeters, W, H, oSize, oMin, oMax); }
}
static void TC hk_hud_frame(void *self, double dt)
{
    minimap_draw();
    ((void (TC *)(void *, double))0x4c5908)(self, dt);
}
static void TC hk_compass_mm(void *compass, void *mat)
{
    if (g_mmDrawn) return; /* the map takes the compass's place */
    ((void (TC *)(void *, void *))g_compassPrev)(compass, mat);
}
static void install_minimap(void)
{
    static const BYTE fn[] = { 0x55, 0x8b, 0xec, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00 };
    if (!c_miniMap) { logf_("mini map: off"); return; }
    if (!rd((void *)0x6775e7, 9) || memcmp((void *)0x6775e7, fn, 9)) { logf_("mini map: map routine not found; disabled"); c_miniMap = 0; return; }
    int a = patch_call_rel(0x477691, 0x4c5908, hk_hud_frame);
    int b = 0;
    if (a) {
        b = patch_call_rel(0x4776b4, 0x6759aa, hk_compass_mm);
        if (!b && patch_call_rel(0x4776b4, (DWORD)dspad_wrap_compass, hk_compass_mm)) { g_compassPrev = (void *)dspad_wrap_compass; b = 1; }
    } else c_miniMap = 0;
    logf_("mini map: frame hook %d compass hook %d", a, b);
}

/* ------------------------------------------------------------------ cursor helpers */
/* The game keeps its own cursor position and feeds it from relative mouse motion (it re-centres
   the OS pointer every frame), so the cursor is steered by nudging the OS pointer by the remaining
   error until the game's cursor reaches the target. */
static int cursor_ui(int *x, int *y)
{
    void *app = AppModule_Get();
    if (!rd(app, 0x140)) return 0;
    *x = (int)(AppModule_GetCursorX(app) / g_uiS);
    *y = (int)(AppModule_GetCursorY(app) / g_uiS);
    return 1;
}
static int g_tgtActive, g_tgtX, g_tgtY;
static DWORD g_tgtStart, g_lastNudge;
static POINT g_lastSet;
static void set_cursor_ui(float x, float y)
{
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > g_uiW - 2) x = (float)(g_uiW - 2);
    if (y > g_uiH - 2) y = (float)(g_uiH - 2);
    g_tgtX = (int)(x + 0.5f);
    g_tgtY = (int)(y + 0.5f);
    g_tgtActive = 1;
    g_tgtStart = timeGetTime();
}
static void nudge_os(int dx, int dy, DWORD now)
{
    POINT os;
    if (!GetCursorPos(&os)) return;
    SetCursorPos(os.x + dx, os.y + dy);
    g_lastSet.x = os.x + dx;
    g_lastSet.y = os.y + dy;
    g_lastNudge = now;
}
static void drive_cursor(DWORD now)
{
    int cx, cy;
    POINT os;
    if (!g_tgtActive) return;
    if (!cursor_ui(&cx, &cy) || now - g_tgtStart > 700) { g_tgtActive = 0; return; }
    int dx = g_tgtX - cx, dy = g_tgtY - cy;
    if (abs(dx) <= 1 && abs(dy) <= 1) { g_tgtActive = 0; return; }
    /* wait until the game has consumed the previous nudge */
    if (GetCursorPos(&os) && os.x == g_lastSet.x && os.y == g_lastSet.y && now - g_lastNudge < 120) return;
    nudge_os((int)(dx * g_uiS), (int)(dy * g_uiS), now);
}

static void nav_move(int dx, int dy)
{
    int cx, cy;
    collect_ui(0);
    if (g_npts == 0) collect_ui(1);
    if (!cursor_ui(&cx, &cy) || g_npts == 0) return;
    if (g_tgtActive) { cx = g_tgtX; cy = g_tgtY; }
    int best = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < g_npts; i++) {
        float vx = (float)(g_pts[i].x - cx), vy = (float)(g_pts[i].y - cy);
        float along = vx * dx + vy * dy;
        float perp = fabsf(vx * dy - vy * dx);
        if (along < 4.0f) continue;
        if (perp > along * 3.0f + 8.0f) continue;
        float score = along + perp * 2.5f;
        if (score < bestScore) { bestScore = score; best = i; }
    }
    static int navLog;
    if (best >= 0) set_cursor_ui(g_pts[best].x, g_pts[best].y);
    if (navLog < 60) {
        int ax = -1, ay = -1;
        POINT sp = { 0, 0 };
        cursor_ui(&ax, &ay);
        GetCursorPos(&sp);
        navLog++;
        logf_("nav dir=(%d,%d) pts=%d from=(%d,%d) -> %s (%d,%d) gamecursor=(%d,%d) os=(%ld,%ld)", dx, dy, g_npts, cx, cy,
              best >= 0 ? g_pts[best].type : "none", best >= 0 ? g_pts[best].x : -1, best >= 0 ? g_pts[best].y : -1, ax, ay,
              sp.x, sp.y);
    }
}
static void dump_ui(void)
{
    static int dumps;
    RECT rc = { 0, 0, 0, 0 }, wr = { 0, 0, 0, 0 };
    POINT sp = { 0, 0 };
    int cx = -1, cy = -1;
    if (dumps++ >= 6) return;
    if (g_hwnd) { GetClientRect(g_hwnd, &rc); GetWindowRect(g_hwnd, &wr); }
    GetCursorPos(&sp);
    cursor_ui(&cx, &cy);
    logf_("ui %dx%d client %ldx%ld window (%ld,%ld)-(%ld,%ld) cursor screen (%ld,%ld) ui (%d,%d) points %d", g_uiW, g_uiH,
          rc.right, rc.bottom, wr.left, wr.top, wr.right, wr.bottom, sp.x, sp.y, cx, cy, g_npts);
    for (int i = 0; i < g_npts && i < 70; i++)
        logf_("  pt %d (%d,%d) %s rect %d,%d,%d,%d", i, g_pts[i].x, g_pts[i].y, g_pts[i].type, g_pts[i].rc.l, g_pts[i].rc.t,
              g_pts[i].rc.r, g_pts[i].rc.b);
}
static void nav_snap_nearest(void)
{
    int cx, cy;
    if (!cursor_ui(&cx, &cy) || g_npts == 0) return;
    int best = -1;
    float bd = 1e12f;
    for (int i = 0; i < g_npts; i++) {
        float vx = (float)(g_pts[i].x - cx), vy = (float)(g_pts[i].y - cy);
        float d = vx * vx + vy * vy;
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) set_cursor_ui(g_pts[best].x, g_pts[best].y);
}

/* ------------------------------------------------------------------ gameplay helpers */
static GopColl g_coll;
static int g_handLogged;

static int go_alive(void *go)
{
    return rd(go, 0x100) && LS_IsAlive(Go_GetLifeState(go));
}

static void *get_party(void)
{
    void *srv = Server_Get();
    if (!rd(srv, 0x20)) return NULL;
    void *party = Server_GetScreenParty(srv);
    return rd(party, 0x100) ? party : NULL;
}

static void *get_leader(void)
{
    void *srv = Server_Get();
    if (!rd(srv, 0x20)) return NULL;
    void *party = get_party();
    void *best = NULL;
    if (party) {
        GopColl *kids = Go_GetChildren(party);
        int n = (int)(kids->e - kids->b);
        void *godb = GoDb_Get();
        void *focus = rd(godb, 0x10) ? GoDb_GetFocusGo(godb) : NULL;
        if (n > 0 && n < 64 && rd(kids->b, n * sizeof(void *))) {
            for (int i = 0; i < n && !best; i++)
                if (rd(kids->b[i], 0x100) && focus && Go_GetGoid(kids->b[i]) == focus) best = kids->b[i];
            for (int i = 0; i < n && !best; i++)
                if (rd(kids->b[i], 0x100) && Go_IsSelected(kids->b[i])) best = kids->b[i];
        }
    }
    if (!best) best = Server_GetScreenHero(srv);
    if (!rd(best, 0x100) || !Go_GetMind(best) || !Go_GetPlacement(best)) return NULL;
    return best;
}

static int go_pos(void *go, SiegePos *out)
{
    void *pl = Go_GetPlacement(go);
    if (!rd(pl, 0x40)) return 0;
    SiegePos *p = GoPlacement_GetPosition(pl);
    if (!rd(p, sizeof *p) || !p->node) return 0;
    *out = *p;
    return 1;
}

/* Camera forward/right on the ground plane, expressed in the node space of `hp`. */
static int cam_basis(const SiegePos *hp, float *fx, float *fz, float *rx, float *rz)
{
    void *eng = SiegeEngine_Get();
    if (!rd(eng, 0x40)) return 0;
    void *cam = SiegeEngine_GetCamera(eng);
    if (!rd(cam, 0x1a0)) return 0;
    SiegePos *csp = Cam_GetCameraSiegePos(cam), *tsp = Cam_GetTargetSiegePos(cam);
    if (!csp->node || !tsp->node) return 0;
    Vec3 d1, d2;
    GetSiegeDifference(&d1, hp, tsp);
    GetSiegeDifference(&d2, hp, csp);
    float x = d1.x - d2.x, z = d1.z - d2.z;
    float len = sqrtf(x * x + z * z);
    if (len < 0.01f || len != len) return 0;
    x /= len;
    z /= len;

    /* Which side is "right" of the camera is a fixed property of the game's world. It used to be
       re-derived every frame from the camera's matrix, and that reading flipped for single frames -
       the hero then faced the mirror image of the stick's direction for that frame (a flicker while
       walking). Now it is fixed; StickXSign=1 in the ini swaps it. */
    int sign = c_stickXSign ? c_stickXSign : -1;
    if (!g_handLogged) {
        g_handLogged = 1;
        logf_("camera basis: StickXSign=%d (set StickXSign=1 in the ini if left and right are swapped)", sign);
    }
    *fx = x;
    *fz = z;
    if (sign > 0) { *rx = z; *rz = -x; }
    else { *rx = -z; *rz = x; }
    return 1;
}

static DWORD g_lastMove;
static float g_lastDx, g_lastDz;
static int g_moving;

static char *go_nema(void *go)
{
    if (!Go_GetAspect || !GoAspect_GetAspectPtr) return NULL;
    void *asp = Go_GetAspect(go);
    if (!rd(asp, 0x48)) return NULL;
    char *nema = GoAspect_GetAspectPtr(asp);
    return rd(nema, 0xc8) ? nema : NULL;
}

/* ---- direct control: the mod moves the hero itself every frame --------------------------- */
static int g_direct;
static void *g_directHero, *g_directGoid;
static DWORD g_directAnimCheck;
static int g_directLogs;
static float g_dt = 0.016f;
static int g_everMoved, g_blockedRun;
static int g_follPosOK = -1, g_follRotOK = -1; /* -1 unknown, 0 layout mismatch, 1 verified */

/* The movement planner remembers where each character's last plan ended and starts the next
   plan from there. After a direct walk that spot is stale, which is what pulled the hero back.
   This rewrites the positions stored in the hero's plan to the current position. */
static int g_planFindOK = -1, g_planNodeOK = -1;
static char *mcp_plan(void *goid)
{
    static const unsigned char sig[] = { 0x55, 0x8b, 0xec, 0x51, 0x56, 0x57, 0x8b, 0x7d, 0x0c, 0x57, 0x8b, 0xf1 };
    if (g_planFindOK < 0) {
        g_planFindOK = rd((void *)0x58b1f1, sizeof sig) && !memcmp((void *)0x58b1f1, sig, sizeof sig);
        if (!g_planFindOK) logf_("direct: plan lookup routine not found; plan sync disabled");
    }
    char *mcp = MCP_Get();
    if (!g_planFindOK || !rd(mcp, 0x20)) return NULL;
    char *it = NULL;
    void *key = goid;
    ((void (TC *)(void *, char **, void **))0x58b1f1)(mcp + 0x14, &it, &key);
    if (!it || it == *(char **)(mcp + 0x18) || !rd(it, 0x14)) return NULL;
    char *plan = *(char **)(it + 0x10);
    if (!rd(plan, 0x130) || *(void **)plan != goid) return NULL;
    return plan;
}
static void plan_sync(void *hero, const SiegePos *pos, int verifyOnly)
{
    char *plan = mcp_plan(Go_GetGoid(hero));
    if (!plan) return;
    char *head = *(char **)(plan + 0xc);
    int n = *(int *)(plan + 0x10);
    if (n <= 0 || n > 512 || !rd(head, 8)) return;
    if (g_planNodeOK < 0 && verifyOnly) {
        char *last = *(char **)(head + 4);
        if (rd(last, 0x3c)) {
            SiegePos *p = (SiegePos *)(last + 0x2c);
            Vec3 d = { 99, 99, 99 };
            if (p->node) GetSiegeDifference(&d, pos, p);
            g_planNodeOK = p->node != 0 && d.x * d.x + d.z * d.z < 9.0f;
            logf_("direct: plan end position %s (%d nodes, %.2f,%.2f,%.2f node %08x vs hero %.2f,%.2f,%.2f node %08x)",
                  g_planNodeOK ? "verified" : "MISMATCH", n, p->x, p->y, p->z, p->node, pos->x, pos->y, pos->z, pos->node);
        }
    }
    if (verifyOnly || g_planNodeOK != 1) return;
    char *node = *(char **)head;
    for (int i = 0; i < n && node != head && rd(node, 0x3c); i++) {
        *(SiegePos *)(node + 0x2c) = *pos;
        node = *(char **)node;
    }
}

/* Last word on the hero's placement: the follower component writes position and facing through
   three call sites. Those calls are redirected here, and while the mod is driving the hero the
   values are replaced with the mod's own, whatever the engine computed. */
static void *g_ovrPlacement;
static int g_ovrPos, g_ovrRot, g_ovrHooked;
static SiegePos g_ovrPosVal;
static float g_ovrQuat[4];
static void TC hk_follower_set_pos(void *pl, SiegePos *p)
{
    if (g_ovrPos && pl == g_ovrPlacement) p = &g_ovrPosVal;
    ((void (TC *)(void *, SiegePos *))0x540dee)(pl, p);
}
static void TC hk_follower_set_rot(void *pl, float *q)
{
    if (g_ovrRot && pl == g_ovrPlacement) q = g_ovrQuat;
    ((void (TC *)(void *, float *))0x540f28)(pl, q);
}
/* While the stick drives the hero, the follower must not change the hero's animation either: the
   shadow path would otherwise restart the walk cycle at every request and switch to standing
   whenever a segment ends. The mod's own walk/stand requests take a different route and still work. */
static void *g_ovrNema;
static int g_ovrAnim, g_choreHooked;
static int TC hk_follower_set_chore(void *aspect, int chore, int stance, int subanim, int flags)
{
    if (g_ovrAnim && aspect == g_ovrNema) return 1;
    return ((int (TC *)(void *, int, int, int, int))0x69ba7c)(aspect, chore, stance, subanim, flags);
}
static int patch_call(DWORD site, DWORD oldTarget, void *newTarget)
{
    BYTE *p = (BYTE *)site;
    DWORD old;
    if (!rd(p, 5) || p[0] != 0xe8 || (DWORD)(site + 5 + *(int *)(p + 1)) != oldTarget) return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int *)(p + 1) = (int)((DWORD)newTarget - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}
static void install_follower_hooks(void)
{
    int a = patch_call(0x606f96, 0x540dee, hk_follower_set_pos);
    int b = patch_call(0x60606e, 0x540dee, hk_follower_set_pos);
    int c = patch_call(0x6075af, 0x540f28, hk_follower_set_rot);
    int d = patch_call(0x6063e4, 0x69ba7c, hk_follower_set_chore);
    int e = patch_call(0x606761, 0x69ba7c, hk_follower_set_chore);
    g_ovrHooked = a && b && c;
    g_choreHooked = d && e;
    logf_("follower hooks: position %d/%d facing %d animation %d/%d", a, b, c, d, e);
}

static int direct_available(void)
{
    if (c_moveMode != 2 || !g_directOK || !Engine_AdjustPointToTerrain) return 0;
    void *world = World_Get();
    return rd(world, 0x10) && !World_IsMultiPlayer(world);
}
static void direct_anim(void *hero, int chore)
{
    void *body = Go_GetBody(hero);
    if (rd(body, 0x40)) GoBody_SAnimate(body, MakeAnimReq(chore));
}
static int g_slide;          /* 1 while moving during a ranged attack: position only, no walk animation or facing */
static int g_savedMayAttack = -1, g_savedBrain = -1, g_savedFidget = -1, g_savedReact;
static BYTE g_savedReactBytes[3];
static void direct_restore_ai(void *hero)
{
    void *mind = Go_GetMind(hero);
    if (g_savedReact && rd(mind, 0x1b8)) {
        memcpy((BYTE *)mind + 0x1a4, g_savedReactBytes, 3);
        g_savedReact = 0;
    }
    if (g_savedBrain == 1 && rd(mind, 0x1b8)) ((BYTE *)mind)[0x14] |= 2;
    if (g_savedFidget == 1 && GoMind_SetAutoFidgets && rd(mind, 0x1b8)) GoMind_SetAutoFidgets(mind, 1);
    g_savedFidget = -1;
    g_savedMayAttack = -1;
    g_savedBrain = -1;
}
/* Takes the hero over for plain walking: no jobs, no auto-attack, walk animation. */
static void direct_start(void *hero)
{
    void *mind = Go_GetMind(hero);
    void *mcp = MCP_Get();
    if (GoMind_GetFrontJob(mind, JQ_ACTION)) GoMind_ClearQ(mind, JQ_ACTION);
    if (rd(mcp, 0x10)) MCP_Flush(mcp, Go_GetGoid(hero), 0.0f);
    if (rd(mind, 0x1b8) && !g_savedReact) {
        /* The hero's reaction switches ("on enemy spotted: attack", "on enemy close: attack", ...)
           are what make the brain start fights or turn to face enemies. Clear them for the walk. */
        memcpy(g_savedReactBytes, (BYTE *)mind + 0x1a4, 3);
        memset((BYTE *)mind + 0x1a4, 0, 3);
        g_savedReact = 1;
    }
    if (GoMind_GetAutoFidgets && GoMind_SetAutoFidgets && rd(mind, 0x1b8) && g_savedFidget < 0) {
        /* idle "fidget" jobs are what kept turning the hero toward the nearest enemy mid-walk */
        g_savedFidget = GoMind_GetAutoFidgets(mind) != 0;
        GoMind_SetAutoFidgets(mind, 0);
    }
    if (c_brainOff && rd(mind, 0x1b8) && g_savedBrain < 0) {
        /* "brain active" flag: with it clear the hero's reflex AI stops queueing its own jobs */
        g_savedBrain = (((BYTE *)mind)[0x14] & 2) != 0;
        ((BYTE *)mind)[0x14] &= ~2;
    }
    direct_anim(hero, CHORE_WALK);
    {
        SiegePos sp;
        if (go_pos(hero, &sp)) plan_sync(hero, &sp, 1);
    }
    g_direct = 1;
    g_slide = 0;
    g_directHero = hero;
    g_directGoid = Go_GetGoid(hero);
    g_directAnimCheck = timeGetTime();
    if (g_directLogs < 30) { g_directLogs++; logf_("direct: start"); }
}
/* Hands the hero back to the engine: stand animation, and the planner re-reads the position. */
static unsigned g_tight[24];
static int g_tightN;
static int tight_piece(unsigned node)
{
    for (int i = 0; i < 24; i++)
        if (g_tight[i] == node) return 1;
    return 0;
}
static void tight_add(unsigned node)
{
    if (!node || tight_piece(node)) return;
    g_tight[g_tightN++ % 24] = node;
    static int logs;
    if (logs < 30) { logs++; logf_("tight terrain piece noted: %08x", node); }
}
static int g_straightRefused; /* the step straight along the stick was refused this frame */
static int g_stopKeepAnim;    /* direct_stop: leave the walk animation alone (the pathfinder takes over mid-stride) */
/* real distance between two positions, whatever terrain pieces they are on */
static float world_dist(const SiegePos *a, const SiegePos *b)
{
    Vec3 wa, wb;
    Vec3 *pa = ((Vec3 *(TC *)(const SiegePos *, Vec3 *))0x671e3c)(a, &wa);
    Vec3 qa = *pa;
    Vec3 *pb = ((Vec3 *(TC *)(const SiegePos *, Vec3 *))0x671e3c)(b, &wb);
    float x = pb->x - qa.x, y = pb->y - qa.y, z = pb->z - qa.z;
    return sqrtf(x * x + y * y + z * z);
}
static int g_bridge;       /* hero is being walked across a seam between two terrain pieces */
static SiegePos g_bridgeL; /* where that crossing lands */
static void direct_stop(void)
{
    g_ovrPos = g_ovrRot = 0;
    g_ovrAnim = 0;
    if (!g_direct) return;
    g_direct = 0;
    void *hero = g_directHero;
    if (rd(hero, 0x100) && Go_GetGoid(hero) == g_directGoid) {
        SiegePos sp;
        if (g_bridge) { g_bridge = 0; GoPlacement_SSetPosition(Go_GetPlacement(hero), &g_bridgeL, 1); } /* never leave the hero over a gap */
        void *mcp = MCP_Get();
        direct_restore_ai(hero);
        if (!g_slide) {
            if (rd(mcp, 0x10)) MCP_Flush(mcp, g_directGoid, 0.0f);
            if (go_pos(hero, &sp)) plan_sync(hero, &sp, 0);
            if (!g_stopKeepAnim) direct_anim(hero, CHORE_DEFAULT);
        }
        if (go_pos(hero, &sp)) plan_sync(hero, &sp, 0);
        if (!g_slide && g_plFace >= 0 && rd(mcp, 0x10)) {
            /* tell the planner the hero now faces this way, or it turns them back to the old plan's facing */
            struct { float q[4]; unsigned node; } rot;
            memcpy(rot.q, GoPlacement_GetOrientation(Go_GetPlacement(hero)), 16);
            rot.node = sp.node;
            MCP_MakeRequestRot(mcp, g_directGoid, g_plFace, &rot);
            if (go_pos(hero, &sp)) plan_sync(hero, &sp, 0);
        }
    }
    g_savedMayAttack = -1;
    g_savedBrain = -1;
    g_savedFidget = -1;
    g_savedReact = 0;
    g_slide = 0;
    if (g_directLogs < 30) { g_directLogs++; logf_("direct: stop"); }
}
/* Which kinds of floor the terrain snap may land on. This used to be a fixed value that happens to
   match ordinary ground only; lifts and other moving platforms are a different kind of floor, so the
   snap refused them. The hero's own movement permissions are what the game itself uses. */
static DWORD terrain_mask(void *hero)
{
    void *body = hero ? Go_GetBody(hero) : NULL;
    DWORD m = rd(body, 0x40) && GoBody_GetPerm ? (DWORD)GoBody_GetPerm(body) : 0;
    static DWORD logged;
    if (m != logged) { logged = m; logf_("terrain snap: hero movement permissions %08x", m); }
    return m | 0x40000000;
}
static DWORD g_blockT; /* since when every direction has been refused */
/* ------------------------------------------------------------------ walk mesh
   The game's pathfinder does not look up "the floor under a coordinate". Each terrain piece carries
   walk meshes (triangles), and each walk mesh carries a list of the walk meshes it is joined to right
   now - on the same piece and on neighbouring pieces. When a lift docks, the platform's mesh is
   joined to that landing's mesh and un-joined from the one it left. Steps are tested against exactly
   this data: the hero stands on a mesh, and a step may land on that mesh or on one joined to it.
   Layout (DSLOA 1.50):
     piece:  [0] array of mesh objects, [4] count, [+0xe0] piece id
     mesh object: [0] mesh data, [4] id (byte), [8] owning piece, [0xc] floor-type flags,
                  [0x10] number of joins (u16), [0x14] joins: { piece id, info* } with info[0] = mesh id
     mesh data:   [4] box min xyz, [0x10] box max xyz, [0x1c] -> { ?, triangles } (0x30 bytes each:
                  three corners), [0x24] leaf count, [0x28] leaves (0x38 bytes each: [0x28] triangle
                  count (u16), [0x2c] triangle indices (u16*)) */
typedef struct { unsigned node; char *nodep; char *ln; } Stand;
static const Stand *g_wm; /* set while a step is being tested against the walk mesh */
static char *wm_piece(unsigned id)
{
    return id ? ((char *(TC *)(void *, unsigned *))0x6711a7)(SiegeEngine_Get(), &id) : NULL;
}
static int wm_mesh_ok(char *lm)
{
    /* every array of a mesh is checked once for readability, then remembered */
    static char *good[48], *bad[16];
    static int ng, nb;
    for (int i = 0; i < 48; i++) if (good[i] == lm) return 1;
    for (int i = 0; i < 16; i++) if (bad[i] == lm) return 0;
    int ok = 0;
    if (rd(lm, 0x34)) {
        int nleaf = *(int *)(lm + 0x24);
        char *leaves = *(char **)(lm + 0x28), *to = *(char **)(lm + 0x1c);
        if (nleaf > 0 && nleaf < 20000 && rd(leaves, nleaf * 0x38) && rd(to, 8) && rd(*(char **)(to + 4), 0x30)) {
            char *tris = *(char **)(to + 4);
            ok = 1;
            for (int i = 0; i < nleaf && ok; i++) {
                char *lf = leaves + i * 0x38;
                int nt = *(WORD *)(lf + 0x28);
                WORD *idx = *(WORD **)(lf + 0x2c);
                if (!nt) continue;
                if (nt > 4000 || !rd(idx, nt * 2)) { ok = 0; break; }
                for (int k = 0; k < nt; k++)
                    if (!rd(tris + idx[k] * 0x30, 0x24)) { ok = 0; break; }
            }
        }
    }
    if (ok) good[ng++ % 48] = lm; else { bad[nb++ % 16] = lm; logf_("walk mesh: unreadable mesh data at %p", lm); }
    return ok;
}
/* is (x,z) over a triangle of this mesh? gives the height there; of several, the one nearest refY */
static WORD g_wmLeaf;   /* leaf of the last triangle wm_on_mesh matched */
static char *g_wmHitLn; /* mesh and leaf of the last spot wm_locate found */
static WORD g_wmHitLeaf;
static int g_wmNoBlock;      /* testing body-width points: objects on the floor are not considered */
static int g_wmHereBlocked;  /* the hero's own spot is marked occupied (he must be able to walk off it) */
static char *g_wmHereLn;
static WORD g_wmHereLeaf;
static int g_wmWhy;          /* why the last spot_ground said no: 1 = no floor there, 2 = floor is occupied */
static int wm_on_mesh(char *ln, float x, float z, float refY, float *yOut)
{
    if (!rd(ln, 0x18)) return 0;
    char *lm = *(char **)ln;
    if (!wm_mesh_ok(lm)) return 0;
    float *bb = (float *)(lm + 4);
    if (x < bb[0] - 0.1f || x > bb[3] + 0.1f || z < bb[2] - 0.1f || z > bb[5] + 0.1f) return 0;
    int nleaf = *(int *)(lm + 0x24), hit = 0;
    char *leaves = *(char **)(lm + 0x28), *tris = *(char **)(*(char **)(lm + 0x1c) + 4);
    float best = 1.6f, bestCentre = 1e9f;
    /* A triangle can be listed in several leaves. The leaf that matters (the "occupied" mark is kept
       per leaf) is the one whose centre is nearest the point, which is how the game picks it too. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nleaf; i++) {
            char *lf = leaves + i * 0x38;
            int nt = *(WORD *)(lf + 0x28);
            WORD *idx = *(WORD **)(lf + 0x2c);
            for (int k = 0; k < nt; k++) {
                float *v = (float *)(tris + idx[k] * 0x30);
                float ax = v[0], az = v[2], bx = v[3], bz = v[5], cx = v[6], cz = v[8];
                float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
                if (d > -1e-6f && d < 1e-6f) continue;
                float u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d;
                float w = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d;
                float q = 1.0f - u - w;
                if (u < -0.01f || w < -0.01f || q < -0.01f) continue;
                float y = u * v[1] + w * v[4] + q * v[7];
                if (pass == 0) {
                    float dy = fabsf(y - refY);
                    if (dy < best) { best = dy; *yOut = y; hit = 1; }
                } else if (fabsf(y - *yOut) < 0.05f) {
                    float *c = (float *)(lf + 0x1c);
                    float ex = c[0] - x, ey = c[1] - y, ez = c[2] - z, cd = ex * ex + ey * ey + ez * ez;
                    if (cd < bestCentre) { bestCentre = cd; g_wmLeaf = *(WORD *)lf; }
                    break; /* this leaf is a candidate; next leaf */
                }
            }
        }
        if (!hit) break;
    }
    return hit;
}
static DWORD wm_perm(void *hero)
{
    void *body = hero ? Go_GetBody(hero) : NULL;
    return rd(body, 0x40) && GoBody_GetPerm ? (DWORD)GoBody_GetPerm(body) : 0xffffffff;
}
/* which mesh is the hero standing on */
static int wm_stand(void *hero, const SiegePos *hp, Stand *s)
{
    char *node = wm_piece(hp->node);
    DWORD perm = wm_perm(hero);
    if (!rd(node, 0xe4)) return 0;
    char **lns = *(char ***)node;
    int n = *(int *)(node + 4);
    if (n <= 0 || n > 64 || !rd(lns, n * sizeof(char *))) return 0;
    float best = 1.2f;
    s->ln = NULL;
    for (int i = 0; i < n; i++) {
        float y;
        if (!rd(lns[i], 0x18) || !(*(DWORD *)(lns[i] + 0xc) & perm)) continue;
        if (!wm_on_mesh(lns[i], hp->x, hp->z, hp->y, &y)) continue;
        if (fabsf(y - hp->y) < best) { best = fabsf(y - hp->y); s->ln = lns[i]; }
    }
    s->node = hp->node;
    s->nodep = node;
    return s->ln != NULL;
}
/* Is this point over the mesh the hero stands on, over one joined to it, or over one joined to
   those? (Two links: the look-ahead point and the body-width points reach a little past the next
   mesh.) On success the point is rewritten as a position on that mesh (piece + height). */
typedef struct { char *ln; unsigned node; char *nodep; } MeshRef;
static int wm_joins(const MeshRef *m, DWORD perm, MeshRef *out, int max)
{
    int n = 0, nc = *(WORD *)(m->ln + 0x10);
    char *conn = *(char **)(m->ln + 0x14);
    if (nc <= 0 || nc > 255 || !rd(conn, nc * 8)) return 0;
    for (int i = 0; i < nc && n < max; i++) {
        unsigned guid = *(unsigned *)(conn + i * 8);
        char *info = *(char **)(conn + i * 8 + 4);
        if (!rd(info, 1)) continue;
        unsigned char id = *(unsigned char *)info;
        char *node = guid == m->node ? m->nodep : wm_piece(guid);
        if (!rd(node, 0xe4)) continue;
        char **lns = *(char ***)node;
        int cnt = *(int *)(node + 4);
        if (cnt <= 0 || cnt > 64 || !rd(lns, cnt * sizeof(char *))) continue;
        for (int k = 0; k < cnt && n < max; k++) {
            if (!rd(lns[k], 0x18) || *(unsigned char *)(lns[k] + 4) != id || !(*(DWORD *)(lns[k] + 0xc) & perm)) continue;
            out[n].ln = lns[k];
            out[n].node = guid;
            out[n].nodep = node;
            n++;
        }
    }
    return n;
}
static int wm_locate(const Stand *s, DWORD perm, SiegePos *p)
{
    SiegePos in = *p;
    if (in.node != s->node) { /* express it on the hero's piece first */
        SiegePos o = { 0, 0, 0, s->node };
        Vec3 d;
        if (!wm_piece(in.node)) return 0;
        GetSiegeDifference(&d, &o, &in);
        in.x = d.x; in.y = d.y; in.z = d.z; in.node = s->node;
    }
    MeshRef set[96];
    int n = 0, first;
    set[n].ln = s->ln; set[n].node = s->node; set[n].nodep = s->nodep; n++;
    n += wm_joins(&set[0], perm, set + n, 40);
    first = n;
    for (int i = 1; i < first && n < 90; i++) n += wm_joins(&set[i], perm, set + n, 96 - n > 12 ? 12 : 96 - n);
    float y, bestDy = 9.0f;
    int found = 0;
    SiegePos out = in;
    unsigned convNode = 0;
    SiegePos conv = in;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int k = 0; k < i; k++) dup |= set[k].ln == set[i].ln;
        if (dup) continue;
        SiegePos q = in;
        if (set[i].node != s->node) {
            if (convNode != set[i].node) {
                SiegePos o = { 0, 0, 0, set[i].node };
                Vec3 d;
                GetSiegeDifference(&d, &o, &in);
                conv.x = d.x; conv.y = d.y; conv.z = d.z; conv.node = set[i].node;
                convNode = set[i].node;
            }
            q = conv;
        }
        if (!wm_on_mesh(set[i].ln, q.x, q.z, q.y, &y)) continue;
        /* the hero's own mesh and its direct joins win over a second-link mesh at similar height */
        float dy = fabsf(y - q.y) + (i >= first ? 0.05f : 0.0f);
        if (dy < bestDy) { bestDy = dy; out = q; out.y = y; found = 1; g_wmHitLn = set[i].ln; g_wmHitLeaf = g_wmLeaf; }
    }
    if (found) *p = out;
    return found;
}
/* Lift zones. Around a lift the engine's floor lookup returns pieces that are not really there (the
   landings of other floors), and no per-step test built on it can be trusted. Wherever that is
   caught happening, the spot is remembered, and within a few metres of it the hero is moved the way
   a mouse click moves him: by an order to the game's own pathfinder, which handles lifts correctly. */
static SiegePos g_zone[8];
static int g_zoneN;
static float world_dist(const SiegePos *a, const SiegePos *b);
static int piece_loaded(unsigned id)
{
    return id && ((void *(TC *)(void *, unsigned *))0x6711a7)(SiegeEngine_Get(), &id) != NULL;
}
static void zone_mark(const SiegePos *p)
{
    if (!p || !piece_loaded(p->node)) return;
    for (int i = 0; i < 8; i++)
        if (g_zone[i].node && piece_loaded(g_zone[i].node) && world_dist(p, &g_zone[i]) < 1.0f) return;
    g_zone[g_zoneN++ % 8] = *p;
    static int logs;
    if (logs < 20) { logs++; logf_("lift zone: floor lookup returned a piece that is not really there; spot remembered (piece %08x)", p->node); }
}
static int in_zone(const SiegePos *hp)
{
    for (int i = 0; i < 8; i++)
        if (g_zone[i].node && piece_loaded(g_zone[i].node) && world_dist(hp, &g_zone[i]) < 3.5f) return 1;
    return 0;
}
/* Terrain snap that cannot be fooled by "phantom" neighbours.
   The engine finds the floor under a point by looking at the point's own terrain piece and then at
   the pieces listed as its neighbours. A lift keeps the landings of every floor on that list, and
   they are laid out as if they were all docked at once - so the snap can pick the landing of another
   floor that only appears to lie underneath. Here the result is accepted only if it is really where
   the point is; otherwise the same point is tried on each neighbouring piece in turn. */
static const SiegePos *g_snapFrom; /* the hero's position, while a step is being worked out */
static void world_of(const SiegePos *p, Vec3 *w)
{
    Vec3 tmp;
    *w = *((Vec3 *(TC *)(const SiegePos *, Vec3 *))0x671e3c)(p, &tmp);
}
static float vdist(const Vec3 *a, const Vec3 *b)
{
    float x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
    return sqrtf(x * x + y * y + z * z);
}
static int add_piece(unsigned *list, int n, unsigned id)
{
    if (!id || n >= 40) return n;
    for (int i = 0; i < n; i++)
        if (list[i] == id) return n;
    list[n] = id;
    return n + 1;
}
static int piece_neighbours(void *eng, unsigned id, unsigned *list, int n)
{
    char *node = ((char *(TC *)(void *, unsigned *))0x6711a7)(eng, &id);
    if (!rd(node, 0x10)) return n;
    char **b = *(char ***)(node + 8), **e = *(char ***)(node + 0xc);
    int cnt = (int)(e - b);
    if (cnt <= 0 || cnt > 64 || !rd(b, cnt * sizeof(char *))) return n;
    for (int i = 0; i < cnt; i++)
        if (rd(b[i], 0x38)) n = add_piece(list, n, *(unsigned *)(b[i] + 0x34));
    return n;
}
/* The "occupied" mark on a floor patch is also set under creatures standing still - a companion a
   step behind the hero, for one - and the pathfinder walks past those. So the mark is not taken as a
   wall when a creature is standing close to the spot; creatures are kept apart by their own circles. */
static float g_blk[40][4];
static int g_nblk;
static float g_act[24][2]; /* every creature near the hero (companions included), relative to him */
static int g_nact;
static int prop_near(const SiegePos *p)
{
    Vec3 d;
    if (!g_snapFrom) return 1;
    GetSiegeDifference(&d, g_snapFrom, p);
    (void)d;
    return 1;
}
static int spot_ground(void *hero, SiegePos *t)
{
    if (g_wm) {
        g_wmWhy = 0;
        if (!wm_locate(g_wm, wm_perm(hero), t)) { g_wmWhy = 1; return 0; }
        /* The game marks the floor under furniture, crates and closed doors as occupied, patch by
           patch. If the hero is somehow standing on an occupied patch he may move about on that
           one patch (to get off it), but not onto another occupied one. */
        if (!g_wmNoBlock && ((bool8 (TC *)(void *, WORD))0x680e78)(g_wmHitLn, g_wmHitLeaf) &&
            !(g_wmHereBlocked && g_wmHitLn == g_wmHereLn && g_wmHitLeaf == g_wmHereLeaf) && prop_near(t)) {
            g_wmWhy = 2;
            return 0;
        }
        return 1;
    }
    void *eng = SiegeEngine_Get();
    DWORD mask = terrain_mask(hero);
    Vec3 w0, w1;
    SiegePos r = *t;
    world_of(t, &w0);
    if (Engine_AdjustPointToTerrain(eng, &r, mask, 2, NULL)) {
        world_of(&r, &w1);
        if (vdist(&w0, &w1) < 1.2f) { *t = r; return 1; }
        if (vdist(&w0, &w1) > 5.0f) zone_mark(g_snapFrom); /* metres off: a lift's other landing, not a ledge */
    }
    /* nothing there, or a phantom: try the real pieces around the hero one by one */
    unsigned list[40];
    int n = 0, first;
    n = add_piece(list, n, t->node);
    if (g_snapFrom) n = add_piece(list, n, g_snapFrom->node);
    first = n;
    for (int i = 0; i < first; i++) n = piece_neighbours(eng, list[i], list, n);
    int second = n;
    for (int i = first; i < second; i++) n = piece_neighbours(eng, list[i], list, n);
    for (int i = 0; i < n; i++) {
        SiegePos o = { 0, 0, 0, list[i] }, q;
        Vec3 d, wq;
        if (!((void *(TC *)(void *, unsigned *))0x6711a7)(eng, &list[i])) continue;
        GetSiegeDifference(&d, &o, t); /* the point, expressed on that piece */
        q.x = d.x; q.y = d.y; q.z = d.z; q.node = list[i];
        world_of(&q, &wq);
        if (vdist(&w0, &wq) > 0.05f) continue; /* conversion did not hold: leave this piece out */
        if (!Engine_AdjustPointToTerrain(eng, &q, mask, 0, NULL) || q.node != list[i]) continue;
        world_of(&q, &w1);
        if (vdist(&w0, &w1) >= 1.2f) continue;
        static int logs;
        if (logs < 12) { logs++; logf_("terrain snap: phantom neighbour avoided, real floor found on piece %08x", q.node); }
        /* where phantoms lurk, the engine's body-width test trips over them too: use the lighter rules */
        tight_add(q.node);
        if (g_snapFrom) tight_add(g_snapFrom->node);
        *t = q;
        return 1;
    }
    return 0;
}
static int spot_walk_r(void *hero, const SiegePos *t, float radius)
{
    void *aiq = AIQuery_Get(), *body = Go_GetBody(hero);
    if (!g_walkCheck || !rd(aiq, 0x10) || !rd(body, 0x40)) return 1;
    return AIQuery_IsAreaWalkable(aiq, GoBody_GetPerm(body), t, 4, radius) != 0;
}
static int spot_walk(void *hero, const SiegePos *t)
{
    return spot_walk_r(hero, t, c_bodyRadius);
}
static int direct_spot_ok(void *hero, SiegePos *t)
{
    return spot_ground(hero, t) && spot_walk(hero, t);
}
/* Walkable floor of the right kind under this point? Objects standing about are NOT considered here
   (see collect_blockers): only the terrain itself. */
static int floor_ok(void *hero, const SiegePos *p)
{
    void *eng = SiegeEngine_Get(), *body = Go_GetBody(hero);
    SiegePos c = *p;
    if (!spot_ground(hero, &c)) return 0;
    if (g_wm || !g_walkCheck || !rd(body, 0x40)) return 1;
    char *lnp = ((char *(TC *)(void *, SiegePos *, DWORD))0x66fd2f)(eng, &c, (DWORD)GoBody_GetPerm(body));
    if (!lnp) return 0;
    if (!g_wmNoBlock && !g_wmHereBlocked && ((bool8 (TC *)(void *, void *, SiegePos *))0x66fe81)(eng, lnp, &c) && prop_near(&c)) return 0;
    return 1;
}
/* room for the body: floor at four points a body-radius around the spot */
static int room_ok(void *hero, const SiegePos *p)
{
    static const float o[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int all = 1;
    g_wmNoBlock = 1;
    for (int i = 0; i < 4 && all; i++) {
        SiegePos q = *p;
        q.x += o[i][0] * c_bodyRadius;
        q.z += o[i][1] * c_bodyRadius;
        if (!floor_ok(hero, &q)) all = 0;
    }
    g_wmNoBlock = 0;
    return all;
}
/* 1 = something is standing on this spot (a crate, a barrel, a closed door: the engine marks the floor
   under objects as occupied); 0 = the floor is free, or is simply not a walkable kind of floor */
static int spot_occupied(void *hero, const SiegePos *t)
{
    void *eng = SiegeEngine_Get(), *body = Go_GetBody(hero);
    DWORD perm = rd(body, 0x40) ? (DWORD)GoBody_GetPerm(body) : 0;
    SiegePos c = *t;
    if (!Engine_AdjustPointToTerrain(eng, &c, perm, 1, NULL)) return 0;
    char *leaf = ((char *(TC *)(void *, SiegePos *, DWORD))0x66fd2f)(eng, &c, perm);
    if (!leaf) return 0;
    return ((bool8 (TC *)(void *, void *, SiegePos *))0x66fe81)(eng, leaf, &c) != 0;
}
/* why the engine calls a spot unwalkable, step by step (for the log) */
static void spot_diag(void *hero, const SiegePos *hp, const SiegePos *t, const char *what)
{
    static int logs;
    static DWORD last;
    DWORD now = timeGetTime();
    if (logs >= 40 || now - last < 400) return;
    last = now;
    logs++;
    void *eng = SiegeEngine_Get(), *body = Go_GetBody(hero);
    DWORD perm = rd(body, 0x40) ? (DWORD)GoBody_GetPerm(body) : 0;
    SiegePos c = *t;
    int snap = Engine_AdjustPointToTerrain(eng, &c, perm, 1, NULL) != 0;
    SiegePos c2 = *t;
    int snapAny = Engine_AdjustPointToTerrain(eng, &c2, 0xffffffff, 1, NULL) != 0;
    char *leaf = snap ? ((char *(TC *)(void *, SiegePos *, DWORD))0x66fd2f)(eng, &c, perm) : NULL;
    char *leafAny = snapAny ? ((char *(TC *)(void *, SiegePos *, DWORD))0x66fd2f)(eng, &c2, 0xffffffff) : NULL;
    int blocked = leaf ? ((bool8 (TC *)(void *, void *, SiegePos *))0x66fe81)(eng, leaf, &c) : -1;
    int blockedAny = leafAny ? ((bool8 (TC *)(void *, void *, SiegePos *))0x66fe81)(eng, leafAny, &c2) : -1;
    logf_("step refused (%s): hero piece %08x spot piece %08x | hero rules: ground %d dy %+.2f leaf %p flags %08x blocked %d | any floor: ground %d piece %08x leaf %p flags %08x blocked %d",
          what, hp->node, t->node, snap, snap ? c.y - t->y : 0.0f, leaf, rd(leaf, 0x10) ? *(DWORD *)(leaf + 0xc) : 0, blocked,
          snapAny, c2.node, leafAny, rd(leafAny, 0x10) ? *(DWORD *)(leafAny + 0xc) : 0, blockedAny);
}
/* Other living characters (not party members) near the hero, as offsets in the hero's node space,
   so the hero cannot walk into or through them. */
static const char *(TC *Go_GetTemplateName)(void *);
static void collect_blockers(void *hero, const SiegePos *hp)
{
    if (!Go_GetTemplateName) Go_GetTemplateName = (void *)GetProcAddress(g_exe, "?GetTemplateName@Go@@QBEPBDXZ");
    g_nblk = 0;
    g_nact = 0;
    void *aiq = AIQuery_Get();
    if (!rd(aiq, 0x10)) return;
    g_coll.e = g_coll.b;
    AIQuery_GetOccupantsInSphere(aiq, hp, c_actorSpacing + 3.0f, &g_coll);
    int n = (int)(g_coll.e - g_coll.b);
    if (n <= 0 || n > 8192) return;
    for (int i = 0; i < n && g_nblk < 40; i++) {
        void *g = g_coll.b[i];
        SiegePos p;
        Vec3 d;
        float keep;
        if (!rd(g, 0x100) || g == hero || !go_pos(g, &p)) continue;
        if (Go_IsActor(g) && go_alive(g) && g_nact < 24) {
            Vec3 ad;
            GetSiegeDifference(&ad, hp, &p);
            g_act[g_nact][0] = ad.x;
            g_act[g_nact][1] = ad.z;
            g_nact++;
        }
        if (Go_IsActor(g)) {
            if (Go_IsScreenPartyMember(g) || !go_alive(g)) continue;
            keep = c_actorSpacing;
        } else {
            /* crates, barrels, levers, closed doors: objects that currently block the way. The engine
               blocks whole floor triangles under them, which on a small platform is most of the
               floor; a circle around the object itself is farPt closer to what is really in the way. */
            char *asp = Go_GetAspect ? Go_GetAspect(g) : NULL;
            if (!rd(asp, 0x48) || !(*(DWORD *)(asp + 0x3c) & 0x40)) continue; /* 0x40: blocks the path right now */
            if (Go_IsInsideInventory(g)) continue;
            float r = ((float (TC *)(void *))0x535fce)(asp) * 0.7f;
            if (!(r > 0.3f)) r = 0.3f;
            if (r > 1.3f) r = 1.3f;
            keep = r + 0.25f;
        }
        GetSiegeDifference(&d, hp, &p);
        if (!Go_IsActor(g)) {
            static void *seen[16];
            static int ns;
            int known = 0;
            for (int k = 0; k < 16; k++) known |= seen[k] == Go_GetGoid(g);
            if (!known) {
                seen[ns++ % 16] = Go_GetGoid(g);
                const char *tn = Go_GetTemplateName ? Go_GetTemplateName(g) : NULL;
                logf_("obstacle: %s, keep-away %.2f m", tn && !IsBadStringPtrA(tn, 64) ? tn : "?", keep);
            }
        }
        g_blk[g_nblk][0] = d.x;
        g_blk[g_nblk][1] = d.z;
        g_blk[g_nblk][2] = keep;
        g_blk[g_nblk][3] = Go_IsActor(g) ? 0.0f : 1.0f;
        g_nblk++;
    }
}
static int blocked_by_actor(float ox, float oz)
{
    for (int i = 0; i < g_nblk; i++) {
        float bx = g_blk[i][0], bz = g_blk[i][1];
        float was = bx * bx + bz * bz;
        float nx = bx - ox, nz = bz - oz;
        float now = nx * nx + nz * nz;
        if (now < g_blk[i][2] * g_blk[i][2] && now < was) return 1;
    }
    return 0;
}
/* slide=1: a ranged attack is running; only the position is driven, the engine keeps the attack
   animation and keeps the hero facing the target. */
/* A lift (or any moving platform) sits next to the floor with a narrow strip between them that has
   no ground at all. The game's pathfinder knows the two pieces are joined and walks across; the
   per-step ground test sees a hole. So when the straight step is refused, look a little further
   along the stick direction: solid, walkable ground on a different terrain piece, at about the same
   height and in plain view, means this is such a seam and the hero is walked across it. */
static int find_bridge(void *hero, const SiegePos *hp, float dx, float dz, SiegePos *out)
{
    void *eng = SiegeEngine_Get(), *aiq = AIQuery_Get(), *body = Go_GetBody(hero);
    static int logs;
    if (!rd(aiq, 0x10) || !rd(body, 0x40)) return 0;
    for (float d = 0.3f; d <= 0.91f; d += 0.15f) {
        SiegePos L = *hp;
        Vec3 df;
        L.x += dx * d;
        L.z += dz * d;
        if (!Engine_AdjustPointToTerrain(eng, &L, terrain_mask(hero), 8, NULL)) continue;
        if (L.node == hp->node) continue;
        if (!AIQuery_IsAreaWalkable(aiq, GoBody_GetPerm(body), &L, 4, 0.15f)) continue;
        GetSiegeDifference(&df, hp, &L);
        if (fabsf(df.y) > 0.6f || df.x * df.x + df.z * df.z > 2.2f * 2.2f) continue;
        SiegePos a = *hp, b = L;
        a.y += 1.0f;
        b.y += 1.0f;
        int los = ((bool8 (TC *)(void *, const SiegePos *, const SiegePos *))0x5ec1fe)(aiq, &a, &b) != 0;
        unsigned na = hp->node, nb = L.node;
        int conn = ((bool8 (TC *)(void *, unsigned *, unsigned *))0x6700a6)(eng, &na, &nb) != 0;
        if (logs < 25) { logs++; logf_("seam: %.2f m ahead, piece %08x -> %08x, height %+.2f, clear view %d, joined %d", d, hp->node, L.node, df.y, los, conn); }
        /* "joined" is the game's own word for it: a lift that is at another floor is still listed as
           a neighbour, but not joined - stepping "onto" it would land the hero on that other floor */
        if (!los || !conn) return 0;
        *out = L;
        return 1;
    }
    return 0;
}
static void place_exact(void *pl, const SiegePos *p)
{
    ((void (TC *)(void *, const SiegePos *))0x540bd4)(pl, p);
}
static int clear_view(const SiegePos *from, const SiegePos *to)
{
    void *aiq = AIQuery_Get();
    SiegePos a = *from, b = *to;
    if (!rd(aiq, 0x10)) return 0;
    a.y += 1.0f;
    b.y += 1.0f;
    return ((bool8 (TC *)(void *, const SiegePos *, const SiegePos *))0x5ec1fe)(aiq, &a, &b) != 0;
}
static void direct_step(void *hero, const SiegePos *hp, float dx, float dz, DWORD now, int slide)
{
    static const float fan[] = { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.4f, -1.4f };
    void *pl = Go_GetPlacement(hero);
    void *body = Go_GetBody(hero);
    void *mind = Go_GetMind(hero);
    char *nema = go_nema(hero);
    if (g_direct && g_directHero != hero) direct_stop();
    if (!g_direct) {
        if (slide) {
            g_direct = 1;
            g_slide = 1;
            g_directHero = hero;
            g_directGoid = Go_GetGoid(hero);
            plan_sync(hero, hp, 1);
            if (g_directLogs < 30) { g_directLogs++; logf_("direct: start (moving while shooting)"); }
        } else {
            direct_start(hero);
        }
    } else if (slide && !g_slide) {
        direct_restore_ai(hero);
        g_slide = 1;
    } else if (!slide && g_slide) {
        direct_start(hero);
    }

    float v = 0.0f;
    if (!slide && nema) v = Aspect_GetCurrentVelocity(nema);
    if (!(v > 0.5f && v < 15.0f)) v = rd(body, 0x40) ? GoBody_GetAvgMoveVelocity(body) : 0.0f;
    if (!(v > 0.5f && v < 15.0f)) v = 4.0f;
    float step = v * c_speedScale * (slide ? c_strafeScale : 1.0f) * g_dt;

    collect_blockers(hero, hp);
    int moved = -1;
    g_straightRefused = 0;
    g_snapFrom = hp;
    Stand stand;
    g_wm = wm_stand(hero, hp, &stand) ? &stand : NULL;
    const Stand *wmStep = g_wm;
    g_wmHereBlocked = 0;
    if (g_wm) {
        SiegePos c = *hp;
        g_wmNoBlock = 1;
        if (wm_locate(g_wm, wm_perm(hero), &c)) {
            g_wmHereBlocked = ((bool8 (TC *)(void *, WORD))0x680e78)(g_wmHitLn, g_wmHitLeaf) != 0;
            g_wmHereLn = g_wmHitLn;
            g_wmHereLeaf = g_wmHitLeaf;
        }
        g_wmNoBlock = 0;
    } else {
        void *eng = SiegeEngine_Get(), *body = Go_GetBody(hero);
        SiegePos c = *hp;
        char *lnp = rd(body, 0x40) ? ((char *(TC *)(void *, SiegePos *, DWORD))0x66fd2f)(eng, &c, (DWORD)GoBody_GetPerm(body)) : NULL;
        if (lnp) g_wmHereBlocked = ((bool8 (TC *)(void *, void *, SiegePos *))0x66fe81)(eng, lnp, &c) != 0;
    }
    {
        static int was = -1;
        static DWORD lastLog;
        int is = g_wm != NULL;
        if (is != was && (was < 0 || now - lastLog > 1000)) {
            was = is;
            lastLog = now;
            logf_(is ? "walk mesh: in use (piece %08x)" : "walk mesh: hero's spot not found on it (piece %08x); old floor test for now", hp->node);
        }
    }
    int hereBad = -1; /* is the spot the hero stands on itself "unwalkable"? (worked out when needed) */
    if (g_bridge == 1) {
        /* once the hero stands on real ground that the walk test merely dislikes, the crossing is over
           and the stick steers again (see the rule in the loop below) */
        SiegePos c = *hp;
        if (spot_ground(hero, &c)) g_bridge = 0;
    }
    if (g_bridge) {
        Vec3 d;
        GetSiegeDifference(&d, hp, &g_bridgeL);
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        if (dist <= step + 0.02f || dist > 3.0f) {
            place_exact(pl, &g_bridgeL);
            g_bridge = 0;
        } else {
            SiegePos t = *hp;
            float k = step / dist;
            t.x += d.x * k;
            t.y += d.y * k;
            t.z += d.z * k;
            place_exact(pl, &t);
        }
        moved = 0;
    }
    for (int i = 0; i < 7 && moved < 0; i++) {
        float c = cosf(fan[i]), s = sinf(fan[i]);
        float ex = dx * c - dz * s, ez = dx * s + dz * c;
        /* sliding along an obstacle is slower the more the hero is turned away from the stick */
        float st = step * c;
        if (blocked_by_actor(ex * st, ez * st)) continue;
        /* the walk mesh decides; if it says no, the older floor test gets a say as well, so anything
           the mesh reading has wrong cannot stop the hero where he used to be able to walk */
        int retried = 0;
        g_wm = wmStep;
    retest:;
        SiegePos t = *hp, probe = *hp;
        t.x += ex * st;
        t.z += ez * st;
        probe.x += ex * (st + c_bodyRadius);
        probe.z += ez * (st + c_bodyRadius);
        int gp = spot_ground(hero, &probe);
        int whyP = g_wm ? g_wmWhy : 0;
        int gt = spot_ground(hero, &t);
        int whyT = g_wm ? g_wmWhy : 0;
        int occupied = whyP == 2 || whyT == 2; /* something stands there: final, no second opinion */
        int crossing = (gp && probe.node != hp->node) || (gt && t.node != hp->node);
        int relaxed = crossing || tight_piece(hp->node);
        /* A step needs: ground under it, room for the body at the new spot, and walkable ground at the
           point a body-radius further on (so the hero stops short of walls). The look-ahead point is
           tested as a point, not as another body-width circle - that asked for twice the room and
           failed all over small platforms such as lifts. */
        int ok = 0;
        /* the first good floor ahead: normally the look-ahead point itself; across a crack between
           two pieces (no floor at all for a few hand-widths, as at a lift sill) a point a little
           further on, provided it is really that close and in plain view (so not through a wall) */
        SiegePos farPt = probe;
        int ahead = gp && floor_ok(hero, &probe), over = 0;
        if (0) {
            static const float more[] = { 0.25f, 0.5f, 0.75f };
            void *aiq = AIQuery_Get();
            for (int k = 0; k < 3 && !ahead && rd(aiq, 0x10); k++) {
                float dd = st + c_bodyRadius + more[k];
                SiegePos q = *hp, a = *hp, b;
                q.x += ex * dd;
                q.z += ez * dd;
                if (!spot_ground(hero, &q) || !floor_ok(hero, &q)) continue;
                if (world_dist(hp, &q) > dd + 0.35f) continue;
                b = q;
                a.y += 1.0f;
                b.y += 1.0f;
                if (!((bool8 (TC *)(void *, const SiegePos *, const SiegePos *))0x5ec1fe)(aiq, &a, &b)) break;
                farPt = q;
                ahead = over = 1;
            }
        }
        if (gt && floor_ok(hero, &t)) {
            /* terrain only: what stands on the floor is handled by the circles in collect_blockers */
            int roomy = room_ok(hero, &t);
            if (roomy && ahead) ok = 1;
            else {
                /* standing on a spot that is itself not proper floor: let him walk off it */
                if (hereBad < 0) { g_wmNoBlock = 1; hereBad = !floor_ok(hero, hp); g_wmNoBlock = 0; }
                if (hereBad) ok = 1;
                /* at a join or a crack the body-width circle hangs over the edge */
                else if (!roomy && ahead && (relaxed || over)) {
                    ok = 1;
                    tight_add(hp->node);
                    tight_add(farPt.node);
                    tight_add(t.node);
                }
            }
        } else if (g_wm && whyT == 1 && ahead && !slide && i == 0 && world_dist(hp, &farPt) < st + c_bodyRadius + 0.4f && clear_view(hp, &farPt)) {
            /* no floor under the next step itself: walk straight over the crack to the good floor */
            static int bl;
            if (bl < 15) { bl++; logf_("crack: walking over to piece %08x (%.2f m)", farPt.node, world_dist(hp, &farPt)); }
            tight_add(hp->node);
            tight_add(farPt.node);
            g_bridgeL = farPt;
            g_bridge = 2;
            moved = 0;
            break;
        }
        if (!ok && g_wm && !retried && !occupied) {
            static int ml;
            static DWORD mlast;
            if (i == 0 && ml < 25 && now - mlast > 400) {
                char buf[400];
                int o = 0, nc = *(WORD *)(g_wm->ln + 0x10);
                char *conn = *(char **)(g_wm->ln + 0x14);
                ml++;
                mlast = now;
                for (int k = 0; k < nc && k < 12 && rd(conn, nc * 8) && o < 360; k++) {
                    char *info = *(char **)(conn + k * 8 + 4);
                    o += snprintf(buf + o, sizeof buf - o, " %08x/%d", *(unsigned *)(conn + k * 8), rd(info, 1) ? *(unsigned char *)info : -1);
                }
                buf[o] = 0;
                logf_("walk mesh refused: on piece %08x mesh %d (flags %08x), step on mesh %d look-ahead on mesh %d; joined to:%s",
                      hp->node, *(unsigned char *)(g_wm->ln + 4), *(DWORD *)(g_wm->ln + 0xc), gt, gp, buf);
            }
            retried = 1;
            g_wm = NULL;
            goto retest;
        }
        if (ok && retried) {
            static int wl;
            static DWORD wlast;
            if (wl < 30 && now - wlast > 500) { wl++; wlast = now; logf_("walk mesh said no, older floor test said yes (piece %08x -> %08x)", hp->node, t.node); }
        }
        if (!ok) {
            if (i == 0) {
                static int dl;
                static DWORD dlast;
                if (dl < 40 && now - dlast > 300) {
                    dl++;
                    dlast = now;
logf_("step refused detail (why: probe %d spot %d; 1 no floor, 2 occupied): ground probe %d spot %d | floor probe %d spot %d | room at spot %d | pieces hero %08x probe %08x spot %08x | hero on floor %d tight %d crossing %d",
                          whyP, whyT, gp, gt, gp ? floor_ok(hero, &probe) : -1, gt ? floor_ok(hero, &t) : -1, gt ? room_ok(hero, &t) : -1,
                          hp->node, probe.node, t.node, floor_ok(hero, hp), tight_piece(hp->node), crossing);
                }
            }
            if (i == 0) g_straightRefused = 1;
            continue;
        }
        if (world_dist(hp, &t) > st + 1.0f) {
            /* the "next step" is really somewhere else entirely (a lift that has moved away is still
               listed next to its old floor): refuse rather than teleport */
            static int jl;
            if (jl < 10) { jl++; logf_("step refused: target is %.1f m away in the world (piece %08x -> %08x)", world_dist(hp, &t), hp->node, t.node); }
            if (i == 0) g_straightRefused = 1;
            continue;
        }
        /* a spot found on the walk mesh is placed exactly as found; the game's usual "set position"
           would look the floor up again by coordinate and can swap in another floor's landing */
        if (g_wm) place_exact(pl, &t);
        else GoPlacement_SSetPosition(pl, &t, 1);
        moved = i;
        break;
    }
    g_wm = NULL;
    /* The character's "follower" component re-applies its own copy of the position and facing
       every frame, so those copies have to be kept in step or the hero is pulled straight back. */
    char *fol = Go_GetFollower(hero);
    if (!rd(fol, 0xa8)) fol = NULL;
    if (fol && g_follPosOK < 0) {
        SiegePos *fp = (SiegePos *)(fol + 0x58);
        float ex = fp->x - hp->x, ey = fp->y - hp->y, ez = fp->z - hp->z;
        g_follPosOK = fp->node == hp->node && ex * ex + ey * ey + ez * ez < 1.0f;
        logf_("direct: follower position copy %s", g_follPosOK ? "verified" : "MISMATCH");
    }
    if (fol && g_follRotOK < 0 && !slide) {
        float *fq = (float *)(fol + 0x74), *pq = GoPlacement_GetOrientation(pl);
        float dot = fq[0] * pq[0] + fq[1] * pq[1] + fq[2] * pq[2] + fq[3] * pq[3];
        g_follRotOK = fabsf(dot) > 0.95f;
        logf_("direct: follower facing copy %s (dot %.3f, orient mode %d)", g_follRotOK ? "verified" : "MISMATCH", dot,
              *(int *)(fol + 0x14));
    }
    if (moved >= 0 && fol && g_follPosOK == 1) {
        SiegePos np;
        if (go_pos(hero, &np)) *(SiegePos *)(fol + 0x58) = np;
    }
    {
        SiegePos np;
        g_ovrPlacement = pl;
        if (go_pos(hero, &np)) { g_ovrPosVal = np; g_ovrPos = 1; }
        if (slide) g_ovrRot = 0;
        g_ovrNema = nema;
        g_ovrAnim = !slide && nema != NULL;
    }
    if (moved >= 0) g_blockT = 0;
    else if (!g_blockT) g_blockT = now ? now : 1;
    if (moved >= 0) { g_everMoved = 1; g_blockedRun = 0; }
    else if (!g_everMoved && g_walkCheck && ++g_blockedRun > 40) {
        /* never managed a single step: the walkability test is evidently wrong for this build */
        g_walkCheck = 0;
        logf_("direct: every direction reported unwalkable from the start; walkability test switched off");
    }
    {
        /* The planner has to see the walk too: area triggers (autosave, region loading, scripted
           events) fire from the planner's boundary checks along planned paths, and a hero moved only
           by direct placement never crosses anything as farPt as it is concerned. So a few times a
           second the planner is asked to plan from where its last plan ended to where the hero now
           is. That "shadow" path is never shown (the placement hooks keep the hero where the stick
           put them); it exists so the engine notices where the hero has been. */
        static DWORD lastShadow;
        static int shadowLogs;
        SiegePos np;
        if (go_pos(hero, &np)) {
            void *mcp = MCP_Get();
            int shadowed = 0;
            if (!slide && c_shadowPlan && g_ovrHooked && g_choreHooked && rd(mcp, 0x10)) {
                if (now - lastShadow >= (DWORD)c_shadowMs) {
                    lastShadow = now;
                    int ret = MCP_MakeRequest(mcp, Go_GetGoid(hero), g_plApproach, &np, 0.0f);
                    const char *rn = MCPRet_ToString(ret);
                    int failed = !rn || IsBadStringPtrA(rn, 64) || strstr(rn, "fail") || strstr(rn, "FAIL");
                    if (shadowLogs < 12) { shadowLogs++; logf_("shadow path request -> %s", rn && !IsBadStringPtrA(rn, 64) ? rn : "?"); }
                    shadowed = !failed;
                } else shadowed = 1;
            }
            if (!shadowed) plan_sync(hero, &np, 0);
        }
    }
    if (slide) {
        static DWORD lastS;
        static int sLogs;
        if (sLogs < 30 && now - lastS > 400) {
            int *job = GoMind_GetFrontJob(mind, JQ_ACTION);
            lastS = now;
            sLogs++;
            logf_("strafe: job %d chore %d/%d moved %d step %.3f", rd(job, 4) ? *job : -1, nema ? Aspect_GetCurrentChore(nema) : -1,
                  nema ? Aspect_GetNextChore(nema) : -1, moved, step);
        }
        return;
    }

    /* Face where the stick points, even when blocked. The follower is put in "fixed direction"
       mode with that direction as its goal, so it stops turning toward a previous target. */
    SiegePos ahead = *hp;
    ahead.x += dx * 3.0f;
    ahead.z += dz * 3.0f;
    GoPlacement_OrientToPosition(pl, &ahead);
    memcpy(g_ovrQuat, GoPlacement_GetOrientation(pl), 16);
    g_ovrRot = 1;
    if (fol && g_follRotOK == 1) {
        float *pq = GoPlacement_GetOrientation(pl);
        SiegePos np;
        if (go_pos(hero, &np)) {
            memcpy(fol + 0x18, pq, 16);
            *(unsigned *)(fol + 0x28) = np.node;
            memcpy(fol + 0x74, pq, 16);
            *(unsigned *)(fol + 0x84) = np.node;
            *(int *)(fol + 0x14) = 2;
        }
    }

    if (now - g_directAnimCheck > 150) {
        g_directAnimCheck = now;
        /* anything the hero's AI queued in the meantime would fight the walk: drop it */
        int *qj = GoMind_GetFrontJob(mind, JQ_ACTION);
        if (qj) {
            static int jatLogs;
            if (jatLogs < 12 && rd(qj, 4)) { jatLogs++; logf_("direct: job type %d was queued during the walk", *qj); }
            void *mcp = MCP_Get();
            GoMind_ClearQ(mind, JQ_ACTION);
            if (rd(mcp, 0x10)) MCP_Flush(mcp, Go_GetGoid(hero), 0.0f);
            static int dropLogs;
            if (dropLogs < 12) { dropLogs++; logf_("direct: dropped a job queued during the walk"); }
        }
        if (nema && Aspect_GetCurrentChore(nema) != CHORE_WALK && Aspect_GetNextChore(nema) != CHORE_WALK) {
            direct_anim(hero, CHORE_WALK);
            if (g_directLogs < 30) { g_directLogs++; logf_("direct: walk animation re-applied"); }
        }
    }
}

static SiegePos g_moveTarget;
static int g_mcpLogs;
/* first=1 starts a normal move job; later refreshes go straight to the movement planner so the
   running walk is re-planned in place instead of being stopped and restarted. */
static int try_move(void *hero, const SiegePos *hp, float dx, float dz, float dist, int how)
{
    static const float fan[] = { 0.0f, 0.3f, -0.3f, 0.65f, -0.65f };
    static const float scale[] = { 1.0f, 0.6f, 0.3f };
    void *mind = Go_GetMind(hero);
    int flushed = 0;
    void *eng = SiegeEngine_Get();
    for (int pass = 0; pass < 3; pass++) {
        float d = dist * scale[pass];
        for (int i = 0; i < 5; i++) {
            float c = cosf(fan[i]), s = sinf(fan[i]);
            SiegePos t = *hp;
            t.x += (dx * c - dz * s) * d;
            t.z += (dx * s + dz * c) * d;
            if (Engine_AdjustPointToTerrain) {
                if (!Engine_AdjustPointToTerrain(eng, &t, terrain_mask(hero), 8, NULL)) continue;
            }
            int direct = 0;
            if (how != 0 && c_moveMode == 1 && g_plApproach >= 0) {
                void *mcp = MCP_Get(), *world = World_Get();
                if (rd(mcp, 0x10) && rd(world, 0x10) && !World_IsMultiPlayer(world)) {
                    /* drop the rest of the current plan, then plan the new heading from where the hero is */
                    if (how == 1 && !flushed) { MCP_Flush(mcp, Go_GetGoid(hero), c_flushDelay); flushed = 1; }
                    int ret = MCP_MakeRequest(mcp, Go_GetGoid(hero), g_plApproach, &t, 0.0f);
                    const char *rn = MCPRet_ToString(ret);
                    int failed = !rn || IsBadStringPtrA(rn, 64) || strstr(rn, "fail") || strstr(rn, "FAIL");
                    if (g_mcpLogs < 25) { g_mcpLogs++; logf_("planner %s d=%.1f fan=%d -> %s", how == 1 ? "turn" : "extend", d, i, failed && !rn ? "?" : rn); }
                    if (failed) continue;
                    direct = 1;
                }
            }
            if (!direct) GoMind_RSMove(mind, &t, QP_CLEAR, AO_HUMAN);
            g_moveTarget = t;
            return 1;
        }
    }
    return 0;
}

static void *g_targetGoid, *g_atkGoid; /* g_atkGoid: last enemy an attack order was given for */
static int g_movedSinceAtk;
static DWORD g_lastAttackCheck;

static void order_attack(void *who, void *enemy)
{
    void *mind = Go_GetMind(who);
    void *inv = Go_GetInventory(who);
    void *sel = rd(inv, 0x40) ? GoInventory_GetSelectedItem(inv) : NULL;
    void *req = NULL;
    if (rd(sel, 0x100) && Go_IsSpell(sel)) {
        void *magic = Go_GetMagic(sel);
        if (rd(magic, 0x20)) {
            if (GoMagic_IsCastableOn(magic, enemy, 1))
                req = MakeJobReq_GG(JAT_CAST, JQ_ACTION, QP_CLEAR, AO_HUMAN, Go_GetGoid(enemy), Go_GetGoid(sel));
            else if (GoMagic_IsCastableOn(magic, who, 1))
                req = MakeJobReq_GG(JAT_CAST, JQ_ACTION, QP_CLEAR, AO_HUMAN, Go_GetGoid(who), Go_GetGoid(sel));
        }
    }
    if (!req) req = MakeJobReq_G(JAT_ATTACK_OBJECT, JQ_ACTION, QP_CLEAR, AO_HUMAN, Go_GetGoid(enemy));
    GoMind_RSDoJob(mind, req);
}
/* When the hero is ordered onto an enemy, companions that are standing about or merely following
   are sent at the same enemy - what selecting the whole party and clicking the enemy does. */
static int free_job(int *job)
{
    int jat = rd(job, 4) ? *job : 0;
    return !job || jat == 0 || jat == 1 || jat == 17 || jat == 18 || jat == 28 || jat == 30;
}
/* the companion's own combat setting: 1 = attack freely, 2 = defend only, 3 = hold fire */
static int combat_orders(void *mind)
{
    return rd(mind, 0x1b4) ? *(int *)((char *)mind + 0x1b0) : 0;
}
static void party_join_attack(void *hero, void *enemy)
{
    static void *lastGoid;
    static DWORD lastT;
    DWORD now = timeGetTime();
    void *party = get_party();
    if (!c_follow || !party || !go_alive(enemy)) return;
    if (!Go_IsActor(enemy)) return;
    if (Go_GetGoid(enemy) == lastGoid && now - lastT < 1500) return;
    lastGoid = Go_GetGoid(enemy);
    lastT = now;
    GopColl *kids = Go_GetChildren(party);
    int n = (int)(kids->e - kids->b);
    if (n <= 1 || n > 64 || !rd(kids->b, n * sizeof(void *))) return;
    for (int i = 0; i < n; i++) {
        void *m = kids->b[i];
        if (m == hero || !rd(m, 0x100) || !go_alive(m)) continue;
        void *mind = Go_GetMind(m);
        if (!rd(mind, 0x70)) continue;
        int *job = GoMind_GetFrontJob(mind, JQ_ACTION);
        if (!(free_job(job) || (rd(job, 4) && *job == JAT_FOLLOW))) continue; /* already doing something of its own */
        {
            static int ol;
            if (ol < 10) { ol++; logf_("companion combat setting: %d (1 attack freely, 2 defend, 3 hold)", combat_orders(mind)); }
        }
        if (combat_orders(mind) != 1) continue; /* set to defend or to hold: not sent in */
        order_attack(m, enemy);
        static int logs;
        if (logs < 15) { logs++; logf_("companion sent at the hero's target %p", Go_GetGoid(enemy)); }
    }
}
static void issue_attack(void *hero, void *enemy)
{
    order_attack(hero, enemy);
    party_join_attack(hero, enemy);
    g_targetGoid = g_atkGoid = Go_GetGoid(enemy);
    g_movedSinceAtk = 0;
}

/* manual target lock (cycled with the right stick click) and the candidate list used for cycling */
typedef struct { void *goid; int kind; float d2; } Cand;
static Cand g_cand[48];
static int g_ncand, g_candRec;
static void *g_lockGoid;
static int g_lockKind;
static void cand_add(void *goid, int kind, float d2)
{
    if (!g_candRec || g_ncand >= 48) return;
    g_cand[g_ncand].goid = goid;
    g_cand[g_ncand].kind = kind;
    g_cand[g_ncand].d2 = d2;
    g_ncand++;
}
static int skipped(void *goid, DWORD now);
static void *pick_enemy(void *hero, const SiegePos *hp, int haveDir, float dx, float dz, void **current)
{
    DWORD nowT = timeGetTime();
    void *mind = Go_GetMind(hero);
    if (current) *current = NULL;
    float radius = c_attackRadius;
    g_coll.e = g_coll.b;
    GoMind_GetEnemiesInSphere(mind, radius, &g_coll);
    int n = (int)(g_coll.e - g_coll.b);
    if (n <= 0 || n > 4096) return NULL;
    void *best = NULL, *lockHit = NULL;
    float bestScore = 1e9f;
    for (int i = 0; i < n; i++) {
        void *g = g_coll.b[i];
        SiegePos ep;
        if (!go_alive(g) || g == hero || !go_pos(g, &ep)) continue;
        /* an enemy behind a closed door or a wall cannot be attacked; leave A free for the door */
        if (GoMind_IsLosClear && !GoMind_IsLosClear(mind, g)) continue;
        if (skipped(Go_GetGoid(g), nowT)) continue;
        if (current && g_targetGoid && Go_GetGoid(g) == g_targetGoid) *current = g;
        Vec3 d;
        GetSiegeDifference(&d, hp, &ep);
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        float score = dist;
        if (haveDir && dist > 0.05f) {
            float c = (d.x * dx + d.z * dz) / dist;
            score = dist * (1.0f + 1.5f * (1.0f - c));
        }
        cand_add(Go_GetGoid(g), 0, dist * dist);
        if (g_lockGoid && g_lockKind == 0 && Go_GetGoid(g) == g_lockGoid) lockHit = g;
        if (score < bestScore) { bestScore = score; best = g; }
    }
    return lockHit ? lockHit : best;
}

/* Returns 1 if an enemy is being attacked. force=1 re-issues even if the target is unchanged. */
static int do_attack(void *hero, const SiegePos *hp, int haveDir, float dx, float dz, int force)
{
    void *mind = Go_GetMind(hero);
    void *current = NULL;
    void *best = pick_enemy(hero, hp, haveDir, dx, dz, &current);
    if (!best) { g_targetGoid = NULL; return 0; }
    if (force) { issue_attack(hero, best); return 1; }
    if (current && GoMind_GetFrontJob(mind, JQ_ACTION)) return 1; /* still busy with the same target */
    issue_attack(hero, current ? current : best);
    return 1;
}

/* Things A was pressed on that did not go away or change: skipped for a while so A can reach
   whatever is behind them (for example an unpickable prop lying in front of a door). */
static struct { void *goid; DWORD until; } g_skip[6];
static void *g_lastUseGoid;
static int g_wantDump;
static DWORD g_lastUseTime;
static int skipped(void *goid, DWORD now)
{
    for (int i = 0; i < 6; i++)
        if (g_skip[i].goid == goid && (int)(g_skip[i].until - now) > 0) return 1;
    return 0;
}
static void note_use(void *goid, DWORD now)
{
    if (goid == g_lastUseGoid && now - g_lastUseTime < 4000 && now - g_lastUseTime > 300) {
        int slot = 0;
        for (int i = 0; i < 6; i++)
            if ((int)(g_skip[i].until - now) <= 0) { slot = i; break; }
        g_skip[slot].goid = goid;
        g_skip[slot].until = now + 12000;
        logf_("A: target %p did not respond twice; skipping it for a while", goid);
        g_wantDump = 1;
        g_lastUseGoid = NULL;
        return;
    }
    g_lastUseGoid = goid;
    g_lastUseTime = now;
}
static int go_clickable(void *g)
{
    void *asp = Go_GetAspect ? Go_GetAspect(g) : NULL;
    if (!rd(asp, 0x48)) return 1;
    if (GoAspect_GetIsVisible && !GoAspect_GetIsVisible(asp)) return 0;
    if (GoAspect_GetIsSelectable && !GoAspect_GetIsSelectable(asp)) return 0;
    return 1;
}
static void dump_nearby(void *hero, const SiegePos *hp);
static void *pick_interact(void *hero, const SiegePos *hp, int *kindOut)
{
    DWORD nowT = timeGetTime();
    void *mind = Go_GetMind(hero);
    void *aiq = AIQuery_Get();
    if (!rd(aiq, 0x10)) return NULL;
    g_coll.e = g_coll.b;
    AIQuery_GetOccupantsInSphere(aiq, hp, c_interactRadius, &g_coll);
    int n = (int)(g_coll.e - g_coll.b);
    if (n <= 0 || n > 8192) return NULL;
    void *best = NULL, *lockHit = NULL;
    int bestKind = 0, lockKind = 0;
    float bestD = 1e9f;
    for (int i = 0; i < n; i++) {
        void *g = g_coll.b[i];
        SiegePos p;
        int kind = 0;
        if (!rd(g, 0x100) || g == hero || !go_pos(g, &p)) continue;
        if (Go_IsBreakable && Go_IsBreakable(g) && !Go_IsActor(g)) {
            kind = 4;
        } else if (!Go_IsActor(g) && Go_IsUsable(g) && !Go_IsInsideInventory(g)) {
            kind = 2; /* usable wins over "item": some doors and levers are also classed as items */
        } else if (Go_IsItem(g)) {
            if (!Go_IsInsideInventory(g)) kind = 1;
        } else if (Go_IsActor(g)) {
            if (!Go_IsScreenPartyMember(g) && go_alive(g) && (Go_HasConversation(g) || Go_HasStore(g)) &&
                !GoMind_IsEnemy(mind, g))
                kind = 3;
        } else if (Go_IsUsable(g)) {
            kind = 2;
        }
        if (!kind) continue;
        if (kind != 3 && !go_clickable(g)) continue; /* the mouse could not click it either */
        if (skipped(Go_GetGoid(g), nowT)) continue;
        Vec3 d;
        GetSiegeDifference(&d, hp, &p);
        float dist = d.x * d.x + d.z * d.z;
        if (kind == 3) dist *= 0.6f; /* slightly prefer talking over stray loot */
        cand_add(Go_GetGoid(g), kind, dist);
        if (g_lockGoid && g_lockKind != 0 && Go_GetGoid(g) == g_lockGoid) { lockHit = g; lockKind = kind; }
        if (kind == 2) dist *= 0.5f; /* doors, chests and levers beat loose items nearby */
        if (dist < bestD) { bestD = dist; best = g; bestKind = kind; }
    }
    if (lockHit) { *kindOut = lockKind; return lockHit; }
    if (best) *kindOut = bestKind;
    return best;
}

static void dump_nearby(void *hero, const SiegePos *hp)
{
    static int dumps;
    void *aiq = AIQuery_Get();
    if (dumps >= 4 || !rd(aiq, 0x10)) return;
    dumps++;
    if (!Go_GetTemplateName) Go_GetTemplateName = (void *)GetProcAddress(g_exe, "?GetTemplateName@Go@@QBEPBDXZ");
    g_coll.e = g_coll.b;
    AIQuery_GetOccupantsInSphere(aiq, hp, 9.0f, &g_coll);
    int n = (int)(g_coll.e - g_coll.b);
    logf_("A: nothing found; %d objects within 9 m:", n);
    for (int i = 0; i < n && i < 40; i++) {
        void *g = g_coll.b[i];
        SiegePos p;
        Vec3 d = { 0, 0, 0 };
        if (!rd(g, 0x100) || g == hero) continue;
        int hasPos = go_pos(g, &p);
        if (hasPos) GetSiegeDifference(&d, hp, &p);
        const char *tn = Go_GetTemplateName ? Go_GetTemplateName(g) : NULL;
        void *asp = Go_GetAspect ? Go_GetAspect(g) : NULL;
        logf_("   %s dist %.1f item %d actor %d usable %d breakable %d inInv %d visible %d selectable %d", tn && !IsBadStringPtrA(tn, 64) ? tn : "?",
              hasPos ? sqrtf(d.x * d.x + d.z * d.z) : -1.0f, Go_IsItem(g), Go_IsActor(g), Go_IsUsable(g),
              Go_IsBreakable ? Go_IsBreakable(g) : -1, Go_IsInsideInventory(g),
              rd(asp, 0x48) && GoAspect_GetIsVisible ? GoAspect_GetIsVisible(asp) : -1,
              rd(asp, 0x48) && GoAspect_GetIsSelectable ? GoAspect_GetIsSelectable(asp) : -1);
    }
}

static int do_interact(void *hero, const SiegePos *hp)
{
    void *mind = Go_GetMind(hero);
    int kind = 0;
    void *best = pick_interact(hero, hp, &kind);
    if (!best) return 0;
    if (kind == 1) note_use(Go_GetGoid(best), timeGetTime());
    if (g_lockGoid && g_lockKind != 0 && Go_GetGoid(best) == g_lockGoid) g_lockGoid = NULL; /* used: lock is done */
    g_atkGoid = NULL;
    if (g_wantDump) { g_wantDump = 0; dump_nearby(hero, hp); }
    if (kind == 4) issue_attack(hero, best);
    else if (kind == 1) GoMind_RSGet(mind, best, QP_CLEAR, AO_HUMAN);
    else if (kind == 2) GoMind_RSUse(mind, best, QP_CLEAR, AO_HUMAN);
    else {
        void *uc = UICommands_Get();
        if (rd(uc, 4)) UICommands_RSTalk(uc, best, hero, 0);
    }
    return 1;
}

/* Highlight what A would act on by drawing the game's own hover ring under it. The ring is drawn
   from inside the game's selection-ring pass (see install_ring_hook). */
static void *g_hlGo, *g_hlGoid;
static int g_hlEnemy, g_hlKind;
static DWORD g_hlLast;
static int g_ringHooked;
static void highlight_clear(void)
{
    g_hlGo = g_hlGoid = NULL;
}
/* One choice for both the ring and the A button: the closest thing wins, enemies get a small
   preference so a monster next to a loot pile is still what gets hit. kind: 0 enemy, 1 item, 2 use, 3 talk */
static void *pick_target(void *hero, const SiegePos *hp, int haveDir, float dx, float dz, int *kind)
{
    void *cur = NULL;
    void *e = pick_enemy(hero, hp, haveDir, dx, dz, &cur);
    if (e && g_lockGoid && g_lockKind != 0) g_lockGoid = NULL; /* a fight started: enemies come first again */
    if (g_lockGoid && g_lockKind != 0) {
        /* a manually chosen door, chest, item or NPC stays the target until it is gone or B is pressed */
        int lk = 0;
        void *li = pick_interact(hero, hp, &lk);
        if (li && Go_GetGoid(li) == g_lockGoid) { *kind = lk; return li; }
        g_lockGoid = NULL;
    }
    if (g_lockGoid && g_lockKind == 0) {
        if (e && Go_GetGoid(e) == g_lockGoid) { *kind = 0; return e; }
        g_lockGoid = NULL; /* dead, out of range or out of sight */
    }
    if (cur) { *kind = 0; return cur; }
    float ed = 1e9f;
    SiegePos p;
    Vec3 d;
    if (e && go_pos(e, &p)) {
        GetSiegeDifference(&d, hp, &p);
        ed = sqrtf(d.x * d.x + d.z * d.z) * 0.75f;
    }
    int ik = 0;
    void *it = pick_interact(hero, hp, &ik);
    float id = 1e9f;
    if (it && go_pos(it, &p)) {
        GetSiegeDifference(&d, hp, &p);
        id = sqrtf(d.x * d.x + d.z * d.z);
    }
    (void)ed;
    (void)id;
    if (e) { *kind = 0; return e; } /* any enemy in view and in range beats loot, doors and NPCs; X still interacts */
    if (it) { *kind = ik; return it; }
    return NULL;
}
/* Stats box for the targeted item on the ground: the same text and the same box the inventory shows
   when the cursor rests on an item, for things that can be equipped only. */
static void *g_infoGoid;
static int g_infoShown;
/* Appends the stats of whatever the controlled hero has equipped in the slot `go` would use.
   `str` is one of the game's wide strings; it is extended with the game's own append routine. */
static void wappend(void **str, const WORD *t, int n)
{
    for (int i = 0; i < n; i++) ((void (TC *)(void *, int, int))0x412a8f)(str, 1, t[i]);
}
static void append_equipped(void **str, void *go)
{
    if (!c_compare || !rd(go, 0x100)) return;
    char *gui = *(char **)((char *)go + 0x28);
    if (!rd(gui, 0x10)) return;
    int slot = ((int (TC *)(void *))0x541c44)(gui);
    if (slot < 0 || slot > 12) return;
    void *hero = get_leader();
    char *inv = hero ? Go_GetInventory(hero) : NULL;
    if (!rd(inv, 0x70)) return;
    int first = slot == 12 ? 8 : slot, last = slot == 12 ? 11 : slot; /* 12 = any ring finger */
    for (int sl = first; sl <= last; sl++) {
        void *eq = *(void **)(inv + 0x34 + sl * 4);
        if (!rd(eq, 0x100) || eq == go) continue;
        char *egui = *(char **)((char *)eq + 0x28);
        if (!rd(egui, 0x10)) continue;
        void *es = NULL;
        ((void *(TC *)(void *, void **))0x541f52)(egui, &es);
        int n = 0;
        if (rd(es, 2)) while (n < 3000 && rd((WORD *)es + n, 2) && ((WORD *)es)[n]) n++;
        if (n > 0) {
            static const WORD head[] = { '\n', ' ', '\n', '-', '-', ' ', 'E', 'q', 'u', 'i', 'p', 'p', 'e', 'd', ' ', '-', '-', '\n' };
            wappend(str, head, sizeof head / sizeof head[0]);
            wappend(str, (const WORD *)es, n);
        }
        if (es) ((void (TC *)(void *))0x402fd9)(&es);
    }
}
/* inventory: the game hands the finished stats text to the hover box here */
static void TC hk_set_tip(void *tb, void *text, DWORD x)
{
    char *mgr = *(char **)0x7ad2c4;
    if (c_compare && !g_shutdown && rd(mgr, 0x18) && text) {
        void *go = NULL;
        ((void *(TC *)(void *, void *))0x53e169)(&go, *(void **)(mgr + 0x14)); /* item under the cursor */
        if (rd(go, 0x100)) append_equipped(&text, go);
    }
    ((void (TC *)(void *, void *, DWORD))0x6fd6d0)(tb, text, x);
}
static char *info_box(void)
{
    void *shell = UIShell_Get ? UIShell_Get() : NULL;
    if (!rd(shell, 0x80)) return NULL;
    char *tb = ((char *(TC *)(void *, const char *, const char *))0x6ea5a4)(shell, "gui_rollover_textbox", NULL);
    return rd(tb, 0x1f0) && rd(*(void **)tb, 0x4c) ? tb : NULL;
}
static void info_visible(char *tb, int on)
{
    ((void (TC *)(void *, int))(*(void ***)tb)[0x48 / 4])(tb, on);
}
static void item_info_hide(void)
{
    if (!g_infoShown) return;
    g_infoShown = 0;
    g_infoGoid = NULL;
    char *tb = info_box();
    if (tb) info_visible(tb, 0);
}
static void item_info_update(void *go, int kind)
{
    if (!c_itemInfo || !g_uiOK) return;
    char *gui = (go && kind == 1) ? *(char **)((char *)go + 0x28) : NULL;
    if (!rd(gui, 0x10)) { item_info_hide(); return; }
    int slot = ((int (TC *)(void *))0x541c44)(gui);
    if (slot < 0 || slot > 12) { item_info_hide(); return; } /* 13 = no slot: potions, gold, scrolls ... */
    char *tb = info_box();
    if (!tb) return;
    void *goid = Go_GetGoid(go);
    if (goid != g_infoGoid || !g_infoShown) {
        struct { void *s; DWORD x; } a = { 0, 0 };
        char dummy = 0;
        void *str = NULL;
        ((void *(TC *)(void *, const void *, void *))0x42a905)(&a, (const void *)0x7aa318, &dummy);
        ((void (TC *)(void *, void *, DWORD))0x6fccb5)(tb, a.s, a.x); /* clear the box */
        ((void *(TC *)(void *, void **))0x541f52)(gui, &str);          /* the item's stat text */
        if (!rd(str, 2) || !*(WORD *)str) {
            if (str) ((void (TC *)(void *))0x402fd9)(&str);
            item_info_hide();
            return;
        }
        append_equipped(&str, go);
        a.s = NULL; a.x = 0;
        ((void *(TC *)(void *, void **, int, int))0x412e27)(&a, &str, 0, -1);
        ((void (TC *)(void *, void *, DWORD))0x6fd6d0)(tb, a.s, a.x);
        ((void (TC *)(void *))0x402fd9)(&str);
        static int logs;
        if (logs < 5) { logs++; logf_("item info: shown for %p (slot %d)", goid, slot); }
        g_infoGoid = goid;
    }
    {
        char *shell = UIShell_Get();
        int *r = (int *)(tb + 0x60);
        int W = *(int *)(shell + 0x60), H = *(int *)(shell + 0x64);
        int w = r[2] - r[0], h = r[3] - r[1];
        if (w > 0 && h > 0 && W > 0 && H > 0) {
            int x = W * c_infoX / 100, y = H * c_infoY / 100 - h / 2;
            if (x + w > W - 4) x = W - 4 - w;
            if (y + h > H - 4) y = H - 4 - h;
            if (x < 4) x = 4;
            if (y < 4) y = 4;
            r[0] = x; r[1] = y; r[2] = x + w; r[3] = y + h;
        }
    }
    info_visible(tb, 1);
    g_infoShown = 1;
}
static void update_highlight(void *hero, const SiegePos *hp, int haveDir, float dx, float dz, DWORD now)
{
    if (!c_highlight || !g_ringHooked) return;
    if (now - g_hlLast < 100) return;
    g_hlLast = now;
    int kind = 0;
    void *t = pick_target(hero, hp, haveDir, dx, dz, &kind);
    g_hlGo = t;
    g_hlGoid = t ? Go_GetGoid(t) : NULL;
    g_hlEnemy = t && kind == 0;
    g_hlKind = kind;
    item_info_update(t, kind);
}

void __cdecl dspad_ring_draw(char *frame)
{
    static int logged;
    void *go = g_hlGo;
    if (!go || !rd(go, 0x100) || Go_GetGoid(go) != g_hlGoid) return;
    char *ui = *(char **)(frame - 4);
    if (!rd(ui, 0xf8) || !rd(Go_GetPlacement(go), 0x40)) return;
    /* ring textures the game itself uses: +0xe4 hostile, +0xdc item, +0xe0 friendly */
    unsigned tex = *(unsigned *)(ui + (g_hlEnemy ? 0xe4 : (g_hlKind == 1 || g_hlKind == 4) ? 0xdc : 0xe0));
    unsigned node = 0;
    if (!logged) { logged = 1; logf_("ring draw: go %p texture %08x", go, tex); }
    ((void (TC *)(void *, void *, unsigned *, unsigned, int))0x4eaf03)(ui, go, &node, tex, 0);
}
void *dspad_ring_return = (void *)0x4eb3a5;
__asm__(".text\n"
        ".globl _dspad_ring_stub\n"
        "_dspad_ring_stub:\n"
        "  pushal\n"
        "  pushl %ebp\n"
        "  call _dspad_ring_draw\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  pushl -0x1c(%ebp)\n"
        "  movl %esi, %ecx\n"
        "  jmp *_dspad_ring_return\n");
extern void dspad_ring_stub(void);
static void install_ring_hook(void)
{
    static const unsigned char site[] = { 0xff, 0x75, 0xe4, 0x8b, 0xce, 0xe8 };
    static const unsigned char fn[] = { 0x55, 0x8b, 0xec, 0x83, 0xec, 0x70 };
    BYTE *p = (BYTE *)0x4eb3a0;
    DWORD old;
    if (!rd(p, 6) || memcmp(p, site, 6) || !rd((void *)0x4eaf03, 6) || memcmp((void *)0x4eaf03, fn, 6)) {
        logf_("selection-ring code not found at expected address; target highlight disabled");
        return;
    }
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0xe9;
    *(int *)(p + 1) = (int)((BYTE *)dspad_ring_stub - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    g_ringHooked = 1;
    logf_("target ring hook installed");
}

/* The name / health readout at the bottom of the screen normally describes whatever is under the
   mouse. While the pad is in use it describes the pad's target instead. The game has just decided
   which object to describe; this replaces that choice (a local variable of its update routine). */
void __cdecl dspad_hover(char *frame)
{
    if (!c_targetBar || g_shutdown || !g_padPlay) return;
    void *go = g_hlGo;
    if (go && rd(go, 0x100) && Go_GetGoid(go) == g_hlGoid) *(void **)(frame - 0x18) = g_hlGoid;
    else *(void **)(frame - 0x18) = *(void **)0x7adc88; /* nothing targeted: nothing described */
}
void *dspad_hover_return = (void *)0x4a2dd3;
__asm__(".text\n"
        ".globl _dspad_hover_stub\n"
        "_dspad_hover_stub:\n"
        "  pushal\n"
        "  pushfl\n"
        "  pushl %ebp\n"
        "  call _dspad_hover\n"
        "  addl $4, %esp\n"
        "  popfl\n"
        "  popal\n"
        "  movl %esi, -0x10(%ebp)\n"
        "  xorl %edi, %edi\n"
        "  jmp *_dspad_hover_return\n");
extern void dspad_hover_stub(void);
/* The pointer image is handed to the renderer once per frame; while the pad is in use it is handed
   "no image" instead. */
static void TC hk_cursor_image(void *rm, DWORD tex, DWORD a, DWORD b)
{
    if (g_hideCursor && !g_shutdown) tex = a = b = 0;
    ((void (TC *)(void *, DWORD, DWORD, DWORD))0x662905)(rm, tex, a, b);
}
static void install_hover_hook(void)
{
    static const unsigned char site[] = { 0x89, 0x75, 0xf0, 0x33, 0xff };
    BYTE *p = (BYTE *)0x4a2dce;
    DWORD old;
    if (!c_targetBar) return;
    if (!rd(p, 5) || memcmp(p, site, 5) || !VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) { logf_("target readout hook: code not as expected; disabled"); return; }
    p[0] = 0xe9;
    *(int *)(p + 1) = (int)((BYTE *)dspad_hover_stub - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf_("target readout hook installed");
}

/* companions (never the controlled hero) drink a potion of their own when health or mana runs low */
static void auto_potions(void *leader, DWORD now)
{
    static DWORD last, lifeAt[64], manaAt[64];
    static float (TC *curLife)(void *), (TC *maxLife)(void *), (TC *curMana)(void *), (TC *maxMana)(void *);
    if (!c_autoPotion || !Go_GetAspect || now - last < 300) return;
    last = now;
    if (!curLife) {
        curLife = (void *)GetProcAddress(g_exe, "?GetCurrentLife@GoAspect@@QBEMXZ");
        maxLife = (void *)GetProcAddress(g_exe, "?GetMaxLife@GoAspect@@QBEMXZ");
        curMana = (void *)GetProcAddress(g_exe, "?GetCurrentMana@GoAspect@@QBEMXZ");
        maxMana = (void *)GetProcAddress(g_exe, "?GetMaxMana@GoAspect@@QBEMXZ");
    }
    if (!curLife || !maxLife || !curMana || !maxMana) return;
    void *party = get_party();
    if (!party) return;
    GopColl *kids = Go_GetChildren(party);
    int n = (int)(kids->e - kids->b);
    if (n <= 1 || n > 64 || !rd(kids->b, n * sizeof(void *))) return;
    float lim = c_autoPotionPct / 100.0f;
    for (int i = 0; i < n; i++) {
        void *m = kids->b[i];
        if (m == leader || !go_alive(m)) continue;
        void *mind = Go_GetMind(m);
        void *asp = Go_GetAspect(m);
        if (!rd(mind, 0x70) || !rd(asp, 0x40)) continue;
        float ml = maxLife(asp), mm = maxMana(asp);
        static int pl;
        if (ml > 1.0f && curLife(asp) < ml * lim && now - lifeAt[i] > 2500) {
            lifeAt[i] = now;
            if (pl < 20) { pl++; logf_("auto potion: companion %d health %.0f of %.0f", i, curLife(asp), ml); }
            GoMind_RSDrinkLife(mind, AO_HUMAN);
        }
        if (mm > 1.0f && curMana(asp) < mm * lim && now - manaAt[i] > 2500) {
            manaAt[i] = now;
            if (pl < 20) { pl++; logf_("auto potion: companion %d mana %.0f of %.0f", i, curMana(asp), mm); }
            GoMind_RSDrinkMana(mind, AO_HUMAN);
        }
    }
}

static void *g_prevLeader;
static DWORD g_lastFollow;
static void update_followers(void *leader, const SiegePos *lp, DWORD now)
{
    if (!c_follow) return;
    int changed = (leader != g_prevLeader);
    if (!changed && now - g_lastFollow < 500) return;
    g_lastFollow = now;
    void *party = get_party();
    if (!party) return;
    GopColl *kids = Go_GetChildren(party);
    int n = (int)(kids->e - kids->b);
    if (n <= 1 || n > 64 || !rd(kids->b, n * sizeof(void *))) { g_prevLeader = leader; return; }
    if (changed) {
        int *job = GoMind_GetFrontJob(Go_GetMind(leader), JQ_ACTION);
        if (rd(job, 4) && *job == JAT_FOLLOW) GoMind_RSStop(Go_GetMind(leader), AO_HUMAN);
    }
    int slot = 0;
    for (int i = 0; i < n; i++) {
        void *m = kids->b[i];
        SiegePos mp;
        if (m == leader || !go_alive(m)) continue;
        void *mind = Go_GetMind(m);
        if (!rd(mind, 0x70) || !go_pos(m, &mp)) continue;
        slot++;
        int *job = GoMind_GetFrontJob(mind, JQ_ACTION);
        int jat = rd(job, 4) ? *job : 0;
        /* standing about counts as free: an idle companion still has a "fidget" (or face / stop / play
           animation) job at the front of its queue */
        int idle = !job || jat == 0 || jat == 1 || jat == 17 || jat == 18 || jat == 28 || jat == 30;
        {
            static int fl;
            static DWORD flast;
            if (fl < 25 && now - flast > 1500) {
                Vec3 d;
                GetSiegeDifference(&d, lp, &mp);
                fl++;
                flast = now;
                logf_("follow: companion %d front job type %d (%s), %.1f m from the hero", slot, jat, idle ? "free" : "busy", sqrtf(d.x * d.x + d.z * d.z));
            }
        }
        {
            /* with enemies about the companion is left to fight: no follow orders, and a follow order
               still running is dropped so its own combat sense takes over */
            int foes = 0;
            g_coll.e = g_coll.b;
            GoMind_GetEnemiesInSphere(mind, 10.0f, &g_coll);
            int ne = (int)(g_coll.e - g_coll.b);
            for (int k = 0; k < ne && ne < 4096 && !foes; k++)
                if (go_alive(g_coll.b[k])) foes = 1;
            int co = combat_orders(mind);
            if (foes && co == 1) { /* attack freely: left to fight */
                if (jat == JAT_FOLLOW) GoMind_RSStop(mind, AO_HUMAN);
                continue;
            }
            if (foes && co == 2) {
                /* defend: no follow order may sit in its queue while enemies are near, or it would never
                   answer an attack; it is only called along once the hero has moved well away */
                Vec3 d;
                GetSiegeDifference(&d, lp, &mp);
                if (d.x * d.x + d.z * d.z < 12.0f * 12.0f) {
                    if (jat == JAT_FOLLOW) {
                        static int dl;
                        if (dl < 10) { dl++; logf_("follow: defending companion %d released from following, enemies near", slot); }
                        GoMind_RSStop(mind, AO_HUMAN);
                    }
                    continue;
                }
            }
        }
        if (!idle && !(changed && jat == JAT_FOLLOW)) continue; /* busy: fighting, looting, already following */
        if (idle) {
            Vec3 d;
            GetSiegeDifference(&d, lp, &mp);
            if (d.x * d.x + d.z * d.z < c_followDist * c_followDist) continue;
        }
        void *req = MakeJobReq_G(JAT_FOLLOW, JQ_ACTION, QP_CLEAR, AO_HUMAN, Go_GetGoid(leader));
        JobReq_SetFloat1(req, 2.0f + 0.5f * slot);
        GoMind_RSDoJob(mind, req);
    }
    g_prevLeader = leader;
}

/* ------------------------------------------------------------------ main update */
enum { MODE_NONE, MODE_GAME, MODE_MENU, MODE_NIS };
static int g_mode = MODE_NONE, g_forceGame;
static DWORD g_lastUiScan, g_lastPadScan, g_xHoldStart;
static int g_menuCached, g_padIndex = -1;
static DWORD g_prevButtons, g_navNext;
static int g_xLongFired;
static DWORD g_orderHoldUntil; /* stick movement is paused briefly after an order so it is not cancelled at once */
static float g_curFx, g_curFy;

/* Camera lock: the game's tracking camera aims at the party object, which trails the group loosely.
   While the option is on, its tracking update is handed the controlled hero instead. Only the tracking
   modes run this code, so cutscenes and scripted camera moves are untouched. */
static float g_camDeadSaved;
static void *TC hk_cam_party(void *server)
{
    void *party = ((void *(TC *)(void *))0x570556)(server);
    char *uic = *(char **)0x7acf84;
    float *dead = rd(uic, 0x80) ? (float *)(uic + 0x7c) : NULL;
    void *hero = (c_camLock && !g_shutdown && party && g_mode != MODE_NIS && g_mode != MODE_NONE) ? get_leader() : NULL;
    if (!hero || !rd(Go_GetPlacement(hero), 0x60)) {
        if (dead && g_camDeadSaved > 0.0f && *dead == 0.0f) *dead = g_camDeadSaved;
        return party;
    }
    if (dead && *dead > 0.0f && *dead < 100.0f) { g_camDeadSaved = *dead; *dead = 0.0f; } /* no slack before the camera moves */
    static int logged;
    if (!logged) { logged = 1; logf_("camera lock active (camera mode %d)", *(int *)uic); }
    return hero;
}
static void install_cam_hook(void)
{
    int ok = c_camLock ? patch_call(0x47ca13, 0x570556, hk_cam_party) : 0;
    logf_("camera lock to leader: option %d hook %d", c_camLock, ok);
}

static float axis(SHORT v)
{
    float f = v / 32767.0f;
    if (f < -1) f = -1;
    return f;
}

static int detect_mode(DWORD now)
{
    void *ws = g_uiOK ? WorldState_Get() : NULL;
    int ingame = 0;
    if (rd(ws, 8)) {
        const char *n = WS_ToString(WorldState_GetCurrentState(ws));
        if (n && !IsBadStringPtrA(n, 48)) {
            static const char *lastN;
            static int stLogs;
            if (n != lastN && stLogs < 300) { stLogs++; lastN = n; logf_("world state: %s", n); }
            if (strstr(n, "nis") || strstr(n, "outro")) return MODE_NIS;
            ingame = strstr(n, "ingame") && !strstr(n, "menu");
        }
    } else if (!g_uiOK) {
        ingame = g_gameOK && rd(Server_Get(), 0x20) && get_leader();
    }
    if (!ingame) return MODE_MENU;
    if (!g_gameOK) return MODE_MENU;
    if (g_forceGame) return MODE_GAME;
    if (now - g_lastUiScan > 120) {
        g_lastUiScan = now;
        g_menuCached = collect_ui(0);
    }
    return g_menuCached > 0 ? MODE_MENU : MODE_GAME;
}

/* Right stick click: step to the next candidate target, nearest first. Enemies are cycled while any
   are in view; otherwise doors, chests, items and NPCs. */
static void cycle_target(void *hero, const SiegePos *hp)
{
    void *prevLock = g_lockGoid;
    int k = 0;
    g_lockGoid = NULL;
    g_ncand = 0;
    g_candRec = 1;
    pick_enemy(hero, hp, 0, 0, 0, NULL);
    if (!g_ncand) pick_interact(hero, hp, &k);
    g_candRec = 0;
    if (!g_ncand) return;
    for (int i = 1; i < g_ncand; i++) { /* insertion sort by distance */
        Cand c = g_cand[i];
        int j = i - 1;
        while (j >= 0 && g_cand[j].d2 > c.d2) { g_cand[j + 1] = g_cand[j]; j--; }
        g_cand[j + 1] = c;
    }
    void *cur = prevLock ? prevLock : g_hlGoid;
    int idx = -1;
    for (int i = 0; i < g_ncand; i++)
        if (g_cand[i].goid == cur) idx = i;
    int next = (idx + 1) % g_ncand;
    g_lockGoid = g_cand[next].goid;
    g_lockKind = g_cand[next].kind;
    g_hlLast = 0; /* move the ring at once */
    static int logs;
    if (logs < 20) { logs++; logf_("target cycle: %d of %d candidates (kind %d)", next + 1, g_ncand, g_lockKind); }
}

/* Attack-while-moving, rhythm version: with the stick and A both held the hero walks normally,
   stops for exactly one attack animation whenever something is in weapon range, then walks on. */
enum { CHORE_ATTACK = 7, CHORE_MAGIC = 8 };
static int g_kite, g_kiteSeen, g_kiteLogs;
static DWORD g_kiteT0, g_kiteMaxMs, g_kiteWalkUntil;

static int kite_update(void *hero, const SiegePos *hp, int haveDir, float dx, float dz, DWORD now)
{
    void *mind = Go_GetMind(hero);
    char *nema = go_nema(hero);
    if (g_kite == 1) {
        int chore = nema ? Aspect_GetCurrentChore(nema) : -1;
        int inAttack = chore == CHORE_ATTACK || chore == CHORE_MAGIC;
        DWORD el = now - g_kiteT0;
        if (inAttack) g_kiteSeen = 1;
        int done = el >= g_kiteMaxMs || (g_kiteSeen && !inAttack && el > 250) ||
                   (!GoMind_GetFrontJob(mind, JQ_ACTION) && el > 250);
        if (!done) return 1; /* planted for this swing or shot */
        g_kite = 2;
        g_kiteWalkUntil = now + (DWORD)c_kiteWalkMs;
        return 0;
    }
    if (g_kite == 2 && (int)(now - g_kiteWalkUntil) < 0) return 0;

    int kind = 0;
    void *tg = pick_target(hero, hp, haveDir, dx, dz, &kind);
    SiegePos tp;
    if (!tg || (kind != 0 && kind != 4) || !go_pos(tg, &tp)) { g_kite = 0; return 0; }
    Vec3 d;
    GetSiegeDifference(&d, hp, &tp);
    float dist = sqrtf(d.x * d.x + d.z * d.z);
    float range = GoMind_GetWeaponRange ? GoMind_GetWeaponRange(mind) : 0.0f;
    if (!(range > 0.3f && range < 80.0f)) range = 1.5f;
    if (range < c_actorSpacing + 0.4f) range = c_actorSpacing + 0.4f; /* melee: reach has to exceed the spacing kept from enemies */
    if (kind == 4 && range < 2.0f) range = 2.0f;
    if (dist > range + 0.7f) { g_kite = 0; return 0; } /* nothing in reach: keep walking */

    /* length of one attack animation, so exactly one swing or shot is taken */
    float dur = 0.0f;
    void *inv = Go_GetInventory(hero);
    void *sel = rd(inv, 0x40) ? GoInventory_GetSelectedItem(inv) : NULL;
    int chore = rd(sel, 0x100) && Go_IsSpell(sel) ? CHORE_MAGIC : CHORE_ATTACK;
    if (nema && Aspect_GetBlender && Aspect_GetCurrentStance && Blender_GetBaseDuration) {
        void *bl = Aspect_GetBlender(nema);
        if (rd(bl, 0x20)) dur = Blender_GetBaseDuration(bl, chore, Aspect_GetCurrentStance(nema));
    }
    if (!(dur > 0.25f && dur < 3.0f)) dur = 1.0f;
    direct_stop();
    issue_attack(hero, tg);
    g_kite = 1;
    g_kiteSeen = 0;
    g_kiteT0 = now;
    g_kiteMaxMs = (DWORD)(dur * 1000.0f) + 200;
    if (g_kiteLogs < 20) {
        g_kiteLogs++;
        void *tm = rd(tg, 0x100) ? Go_GetMind(tg) : NULL;
        logf_("attack on the move: kind %d dist %.2f range %.2f animation %.2fs (hero melee engage %.2f personal space %.2f, target personal space %.2f)",
              kind, dist, range, dur, GoMind_GetMeleeEngageRange ? GoMind_GetMeleeEngageRange(mind) : -1.0f,
              GoMind_GetPersonalSpaceRange ? GoMind_GetPersonalSpaceRange(mind) : -1.0f,
              GoMind_GetPersonalSpaceRange && rd(tm, 0x1b8) ? GoMind_GetPersonalSpaceRange(tm) : -1.0f);
    }
    return 1;
}

static int g_zDrive;
static float g_zDx, g_zDz;
static DWORD g_zLast;
static SiegePos g_zTarget;
static void game_update(DWORD now, DWORD btn, DWORD pressed, float lx, float ly, float rx, float ry)
{
    void *hero = get_leader();
    SiegePos hp;
    if (!hero || !go_pos(hero, &hp)) { direct_stop(); return; }
    void *mind = Go_GetMind(hero);
    int alive = go_alive(hero);
    int lt = (btn & B_LT) != 0;
    {
        /* while the game is paused the hero must not be moved or given orders */
        void *app = AppModule_Get ? AppModule_Get() : NULL;
        if (AppModule_IsUserPaused && rd(app, 0x140) && AppModule_IsUserPaused(app)) {
            direct_stop();
            g_moving = 0;
            alive = 0;
        }
    }

    /* camera */
    hold_key(k_camLeft, rx < -0.5f);
    hold_key(k_camRight, rx > 0.5f);
    hold_key(k_camUp, ry > 0.5f);
    hold_key(k_camDown, ry < -0.5f);
    hold_key(k_zoomIn, !lt && (btn & B_UP));
    hold_key(k_zoomOut, !lt && (btn & B_DOWN));

    if (lt) {
        static const DWORD order[8] = { B_A, B_B, B_X, B_Y, B_UP, B_RIGHT, B_DOWN, B_LEFT };
        for (int i = 0; i < 8; i++)
            if (pressed & order[i]) tap_key(k_awp[i]);
        if (pressed & B_LB) tap_key(k_selectAll);
        if (pressed & B_RB) tap_key(k_spellbook);
        if (pressed & B_BACK) tap_key(k_journal);
    } else {
        void *uig = UIGame_Get();
        if ((pressed & B_RIGHT) && rd(uig, 4)) UIGame_SelectNextPlayer(uig);
        if ((pressed & B_LEFT) && rd(uig, 4)) UIGame_SelectLastPlayer(uig);
        if ((pressed & B_LB) && alive) GoMind_RSDrinkLife(mind, AO_HUMAN);
        if ((pressed & B_RB) && alive) GoMind_RSDrinkMana(mind, AO_HUMAN);
        if (pressed & B_Y) tap_key(k_weaponCycle);
        if (pressed & B_BACK) tap_key(k_inventory);
    }
    if (pressed & B_START) tap_key(k_menu);
    if (pressed & B_L3) tap_key(k_map);
    if (pressed & B_R3) {
        if (lt) tap_key(k_pause);
        else if (alive) cycle_target(hero, &hp);
    }

    /* stick direction in hero node space */
    float mag = sqrtf(lx * lx + ly * ly);
    float dx = 0, dz = 0;
    int haveDir = 0;
    if (mag > c_deadzone) {
        float fx, fz, rgx, rgz;
        if (cam_basis(&hp, &fx, &fz, &rgx, &rgz)) {
            dx = fx * ly + rgx * lx;
            dz = fz * ly + rgz * lx;
            float l = sqrtf(dx * dx + dz * dz);
            if (l > 0.001f) { dx /= l; dz /= l; haveDir = 1; }
        }
    }

    int attackHeld = !lt && (btn & (B_A | B_RT));
    int attackPressed = !lt && (pressed & (B_A | B_RT));
    int attacking = 0;
    /* with a bow or a spell selected the hero may keep moving while attacking */
    int canStrafe = 0;
    int rhythm = c_strafe == 2 && haveDir && alive && attackHeld && direct_available();
    if (c_strafe == 1 && haveDir && direct_available()) {
        void *inv = Go_GetInventory(hero);
        void *sel = rd(inv, 0x40) ? GoInventory_GetSelectedItem(inv) : NULL;
        canStrafe = rd(sel, 0x100) && (Go_IsSpell(sel) || (Go_IsRangedWeapon && Go_IsRangedWeapon(sel)));
    }
    if (attackPressed) {
        static int aLogs;
        void *inv = Go_GetInventory(hero);
        void *sel = rd(inv, 0x40) ? GoInventory_GetSelectedItem(inv) : NULL;
        if (aLogs < 20) {
            aLogs++;
            logf_("attack pressed: stick %d selected item %p spell %d ranged %d -> move while attacking %d", haveDir, sel,
                  rd(sel, 0x100) ? Go_IsSpell(sel) : -1, rd(sel, 0x100) && Go_IsRangedWeapon ? Go_IsRangedWeapon(sel) : -1,
                  canStrafe);
        }
    }
    if (rhythm) {
        attacking = kite_update(hero, &hp, haveDir, dx, dz, now);
        g_lastAttackCheck = now;
        if (!attacking && attackPressed) {
            int tk = 0;
            void *tg = pick_target(hero, &hp, haveDir, dx, dz, &tk);
            if (tg && tk != 0 && tk != 4) {
                direct_stop();
                do_interact(hero, &hp);
                g_orderHoldUntil = now + 1200;
            }
        }
    } else if (alive && attackPressed) {
        g_kite = 0;
        int tk = 0;
        void *tg = pick_target(hero, &hp, haveDir, dx, dz, &tk);
        if (!(canStrafe && tg && tk == 0)) direct_stop();
        else if (g_direct && !g_slide) direct_restore_ai(hero);
        if (tg && tk == 0) {
            /* pressing A again on the enemy already being attacked must not restart the swing */
            int busy = GoMind_GetFrontJob(mind, JQ_ACTION) != NULL;
            int same = g_atkGoid && Go_GetGoid(tg) == g_atkGoid && busy;
            {
                /* the previous attack order on this enemy died at once: it is unreachable for now */
                static void *lastGoid;
                static DWORD lastT;
                void *gid = Go_GetGoid(tg);
                if (!same && !busy && !g_movedSinceAtk && gid == lastGoid && now - lastT > 250 && now - lastT < 2500) {
                    int slot = 0;
                    for (int i = 0; i < 6; i++)
                        if ((int)(g_skip[i].until - now) <= 0) { slot = i; break; }
                    g_skip[slot].goid = gid;
                    g_skip[slot].until = now + 6000;
                    logf_("A: attack on %p keeps failing; skipping that enemy for a while", tg);
                    int k2 = 0;
                    tg = pick_target(hero, &hp, haveDir, dx, dz, &k2);
                    tk = k2;
                }
                lastGoid = gid;
                lastT = now;
            }
            if (tg && tk != 0) {
                direct_stop();
                do_interact(hero, &hp);
                g_orderHoldUntil = now + 1200;
            } else if (tg && !same) issue_attack(hero, tg);
            attacking = tg && tk == 0;
            static int pressLogs;
            if (pressLogs < 30) { pressLogs++; logf_("A: enemy %p %s", tg, same ? "(already attacking, left alone)" : "attack issued"); }
        } else if (tg) {
            do_interact(hero, &hp);
            g_orderHoldUntil = now + 1200;
            static int useLogs;
            if (useLogs < 30) { useLogs++; logf_("A: interact kind %d target %p", tk, tg); }
        } else {
            static int noneLogs;
            if (noneLogs < 10) { noneLogs++; logf_("A: nothing in range"); }
            dump_nearby(hero, &hp);
        }
        g_lastAttackCheck = now;
        g_moving = 0;
    } else if (alive && attackHeld && g_targetGoid) {
        attacking = 1;
        if (now - g_lastAttackCheck > 250) {
            g_lastAttackCheck = now;
            attacking = do_attack(hero, &hp, haveDir, dx, dz, 0);
        }
    }
    if (!attackHeld) { g_targetGoid = NULL; g_kite = 0; }

    /* X: tap = pick up / use / talk to nearest thing, hold = whole party collects loot */
    if (!lt) {
        if (pressed & B_X) { g_xHoldStart = now; g_xLongFired = 0; }
        if ((btn & B_X) && !g_xLongFired && now - g_xHoldStart > 450) { g_xLongFired = 1; tap_key(k_loot); }
        if (!(btn & B_X) && (g_prevButtons & B_X) && !g_xLongFired && alive) {
            direct_stop();
            if (do_interact(hero, &hp)) g_orderHoldUntil = now + 1200;
        }
        if ((pressed & B_B) && alive) { direct_stop(); GoMind_RSStop(mind, AO_HUMAN); g_moving = 0; g_lockGoid = NULL; g_atkGoid = NULL; }
    }

    /* movement */
    if (g_orderHoldUntil) {
        if (!haveDir || (int)(now - g_orderHoldUntil) >= 0 || !GoMind_GetFrontJob(mind, JQ_ACTION)) g_orderHoldUntil = 0;
        else haveDir = 0;
    }
    if (direct_available()) {
        if (alive && haveDir && (!attacking || canStrafe)) {
            /* Crossing help. The per-step ground test cannot be trusted where terrain pieces meet at
               a lift; the game's own pathfinder can (it knows whether the lift is there, and how to
               get on it). So when the straight step is refused and open, walkable ground on another
               piece lies just ahead, the pathfinder walks the hero there and the stick takes over
               again the moment he arrives. */
            g_movedSinceAtk = 1;
            if (c_liftZone && in_zone(&hp) && !attacking) {
                Vec3 zd;
                float left = 99.0f;
                if (g_direct) {
                    g_stopKeepAnim = 1; /* keep walking through the hand-over */
                    direct_stop();
                    g_stopKeepAnim = 0;
                    g_zDrive = 0;
                    static int zl;
                    if (zl < 10) { zl++; logf_("lift zone: pathfinder takes over"); }
                }
                if (g_zDrive && piece_loaded(g_zTarget.node)) {
                    GetSiegeDifference(&zd, &hp, &g_zTarget);
                    left = sqrtf(zd.x * zd.x + zd.z * zd.z);
                }
                int turned = dx * g_zDx + dz * g_zDz < 0.92f;
                int idle = !GoMind_GetFrontJob(mind, JQ_ACTION);
                if (!g_zDrive || turned || ((left < 1.3f || idle) && now - g_zLast > 250)) {
                    static const float dist[] = { 4.5f, 3.5f, 2.6f, 1.8f, 1.2f, 0.7f };
                    void *eng = SiegeEngine_Get();
                    for (int k = 0; k < 6; k++) {
                        SiegePos t = hp;
                        t.x += dx * dist[k];
                        t.z += dz * dist[k];
                        if (!Engine_AdjustPointToTerrain(eng, &t, terrain_mask(hero), 8, NULL)) continue;
                        if (fabsf(world_dist(&hp, &t) - dist[k]) > 0.6f) continue; /* not really there */
                        GoMind_RSMove(mind, &t, QP_CLEAR, AO_HUMAN);
                        g_zTarget = t;
                        break;
                    }
                    g_zDrive = 1;
                    g_zDx = dx;
                    g_zDz = dz;
                    g_zLast = now;
                }
            } else {
                if (g_zDrive) {
                    g_zDrive = 0;
                    static int zl;
                    if (zl < 10) { zl++; logf_("lift zone: left, stick control again"); }
                }
                direct_step(hero, &hp, dx, dz, now, attacking);
            }
            g_moving = 0;
        } else {
            if (g_zDrive) { g_zDrive = 0; if (alive && !attacking) GoMind_RSStop(mind, AO_HUMAN); }
            direct_stop();
        }
    } else if (alive && haveDir && !attacking) {
        float turn = dx * g_lastDx + dz * g_lastDz;
        int how = g_moving ? -1 : 0; /* 0 start, 1 turn (replace plan), 2 extend (append), -1 nothing */
        if (g_moving) {
            Vec3 rem;
            GetSiegeDifference(&rem, &hp, &g_moveTarget);
            float left = sqrtf(rem.x * rem.x + rem.z * rem.z);
            if (turn < 0.99f && now - g_lastMove >= (DWORD)c_moveIntervalMs) how = 1;
            else if (left < c_moveDist * 0.6f && now - g_lastMove >= 200) how = 2;
        }
        if (how >= 0 && try_move(hero, &hp, dx, dz, how == 2 ? c_moveDist * 1.2f : c_moveDist, how)) {
            g_moving = 1;
            g_lastMove = now;
            if (how != 2) { g_lastDx = dx; g_lastDz = dz; }
        }
    } else if (g_moving && !haveDir) {
        if (alive && !attacking) GoMind_RSStop(mind, AO_HUMAN);
        g_moving = 0;
    }

    update_highlight(hero, &hp, haveDir, dx, dz, now);
    update_followers(hero, &hp, now);
    auto_potions(hero, now);
}

static void menu_update(DWORD now, float dt, DWORD btn, DWORD pressed, float rx, float ry, int entered)
{
    if (entered) {
        collect_ui(0);
        dump_ui();
        nav_snap_nearest();
    }
    /* d-pad / left stick: snap navigation with auto-repeat */
    DWORD dirs = btn & (B_UP | B_DOWN | B_LEFT | B_RIGHT);
    if (btn & N_UP) dirs |= B_UP;
    if (btn & N_DOWN) dirs |= B_DOWN;
    if (btn & N_LEFT) dirs |= B_LEFT;
    if (btn & N_RIGHT) dirs |= B_RIGHT;
    static DWORD prevDirs;
    if (dirs && (dirs != prevDirs || now >= g_navNext)) {
        g_navNext = now + (dirs != prevDirs ? c_navRepeatMs * 2 : c_navRepeatMs);
        if (dirs & B_UP) nav_move(0, -1);
        else if (dirs & B_DOWN) nav_move(0, 1);
        else if (dirs & B_LEFT) nav_move(-1, 0);
        else if (dirs & B_RIGHT) nav_move(1, 0);
    }
    prevDirs = dirs;

    /* right stick: free cursor */
    float m = sqrtf(rx * rx + ry * ry);
    if (m > c_deadzone) {
        float sp = c_cursorSpeed * (g_uiW / 800.0f) * m * dt;
        g_curFx += rx / m * sp;
        g_curFy -= ry / m * sp;
        int ix = (int)g_curFx, iy = (int)g_curFy;
        g_tgtActive = 0;
        if (ix || iy) {
            g_curFx -= ix;
            g_curFy -= iy;
            nudge_os(ix, iy, now);
        }
    } else {
        g_curFx = g_curFy = 0;
    }
    drive_cursor(now);
    {
        static DWORD lastTrace;
        static int traces;
        int gx, gy;
        if (g_tgtActive && traces < 40 && now - lastTrace > 40 && cursor_ui(&gx, &gy)) {
            lastTrace = now;
            traces++;
            logf_("cursor drive: game (%d,%d) target (%d,%d)", gx, gy, g_tgtX, g_tgtY);
        }
    }

    static int clickLog;
    if ((pressed & (B_A | B_X | B_UP | B_DOWN | B_LEFT | B_RIGHT | N_UP | N_DOWN | N_LEFT | N_RIGHT)) && clickLog < 40) {
        clickLog++;
        logf_("menu input: buttons %05lx pressed %05lx", (unsigned long)btn, (unsigned long)pressed);
    }
    mouse_button(0, (btn & B_A) != 0);
    mouse_button(1, (btn & B_X) != 0);
    hold_key(VK_SHIFT, (btn & B_LT) != 0);
    hold_key(VK_CONTROL, (btn & B_RT) != 0);
    if (pressed & (B_B | B_START)) tap_key(k_menu);
    if (pressed & B_BACK) tap_key(k_inventory);
    if (pressed & B_Y) tap_key(k_spellbook);
    if (pressed & B_L3) tap_key(k_map);
    if (pressed & B_R3) tap_key(k_pause);
    if (g_gameOK && (pressed & (B_LB | B_RB)) && g_mode == MODE_MENU) {
        void *hero = rd(Server_Get(), 0x20) ? get_leader() : NULL;
        if (hero && go_alive(hero)) {
            if (pressed & B_LB) GoMind_RSDrinkLife(Go_GetMind(hero), AO_HUMAN);
            if (pressed & B_RB) GoMind_RSDrinkMana(Go_GetMind(hero), AO_HUMAN);
        }
    }
}

static int g_inited, g_inTick, g_enabled;
static DWORD g_lastTick, g_mainThread;

static void init_once(void)
{
    g_inited = 1;
    load_config();
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char *base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    logf_("DSPad 1.0 starting in %s", base);
    if (lstrcmpiA(base, "DSLOA.exe")) {
        logf_("not DSLOA.exe - controller support disabled");
        return;
    }
    memset(g_typeKind, -1, sizeof g_typeKind);
    resolve();
    if (g_gameOK && c_highlight) install_ring_hook();
    if (g_gameOK && g_directOK) install_follower_hooks();
    if (g_gameOK) install_cam_hook();
    install_ui_scale();
    if (g_gameOK) install_minimap();
    if (g_gameOK) install_hover_hook();
    if (c_hideCursor) logf_("cursor hide hook: %d", patch_call(0x47784f, 0x662905, hk_cursor_image));
    if (g_gameOK && c_compare) logf_("equipped comparison hook: %d", patch_call(0x505af7, 0x6fd6d0, hk_set_tip));
    static const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !XGetState; i++) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h) {
            XGetState = (void *)GetProcAddress(h, "XInputGetState");
            if (XGetState) logf_("using %s", dlls[i]);
        }
    }
    if (!XGetState) { logf_("XInput not available"); return; }
    if (!g_gameOK && !g_uiOK) { logf_("no engine functions found - disabled"); return; }
    g_enabled = 1;
}

static void tick(void)
{
    if (g_inTick || g_shutdown) return;
    DWORD now = timeGetTime();
    if (now - g_lastTick < 5) return;
    g_inTick = 1;
    float dt = (now - g_lastTick) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_dt = dt;
    g_lastTick = now;
    if (!g_inited) { g_mainThread = GetCurrentThreadId(); init_once(); logf_("first tick on thread %lu, enabled=%d", g_mainThread, g_enabled); }
    /* the scaler must keep working even with no controller connected */
    if (g_uiScaleHooked && GetCurrentThreadId() == g_mainThread) ensure_d3d_hook();
    if (!g_enabled || GetCurrentThreadId() != g_mainThread) { g_inTick = 0; return; }

    /* controller */
    XSTATE xs;
    int ok = 0;
    if (g_padIndex >= 0) ok = XGetState(g_padIndex, &xs) == 0;
    if (!ok) {
        g_padIndex = -1;
        if (now - g_lastPadScan > 1500 || !g_lastPadScan) {
            g_lastPadScan = now ? now : 1;
            static int warned;
            if (!warned++) logf_("looking for an XInput controller...");
            for (int i = 0; i < 4 && !ok; i++) {
                if (c_pad >= 0 && i != c_pad) continue;
                if (XGetState(i, &xs) == 0) { ok = 1; g_padIndex = i; logf_("controller %d connected", i); }
            }
        }
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (!ok || pid != GetCurrentProcessId()) {
        release_all();
        if (g_gameOK) direct_stop();
        g_prevButtons = 0;
        g_moving = 0;
        g_inTick = 0;
        return;
    }
    g_hwnd = fg;

    float lx = axis(xs.pad.lx), ly = axis(xs.pad.ly), rx = axis(xs.pad.rx), ry = axis(xs.pad.ry);
    if (c_swapSticks) { float t = lx; lx = rx; rx = t; t = ly; ly = ry; ry = t; }
    DWORD btn = xs.pad.buttons;
    if (xs.pad.lt > 60) btn |= B_LT;
    if (xs.pad.rt > 60) btn |= B_RT;
    if (ly > 0.55f) btn |= N_UP;
    if (ly < -0.55f) btn |= N_DOWN;
    if (lx < -0.55f) btn |= N_LEFT;
    if (lx > 0.55f) btn |= N_RIGHT;
    DWORD pressed = btn & ~g_prevButtons;

    int mode = detect_mode(now);
    int entered = (mode != g_mode);
    if (entered) {
        release_all();
        g_moving = 0;
        g_targetGoid = NULL;
        g_tgtActive = 0;
        if (g_gameOK) direct_stop();
        if (g_gameOK) highlight_clear();
        if (g_gameOK) item_info_hide();
        logf_("mode -> %s %s", mode == MODE_GAME ? "gameplay" : mode == MODE_MENU ? "menu" : "cutscene",
              mode == MODE_MENU ? g_menuNames : "");
        g_mode = mode;
    }
    {
        /* mouse pointer: hidden during play while the pad is the device in use; any mouse movement or
           click brings it back, and menus always show it */
        static POINT rest, last;
        static int same;
        POINT p;
        int mouseAct = 0;
        int padAct = btn != 0 || fabsf(lx) > c_deadzone || fabsf(ly) > c_deadzone || fabsf(rx) > c_deadzone || fabsf(ry) > c_deadzone;
        if (GetCursorPos(&p)) {
            if (p.x == last.x && p.y == last.y) { if (++same >= 10) rest = p; } else same = 0;
            last = p;
            if (abs(p.x - rest.x) > 3 || abs(p.y - rest.y) > 3) mouseAct = 1;
        }
        if (mode == MODE_GAME) {
            if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) mouseAct = 1;
            if (mouseAct) g_padLast = 0;
            else if (padAct) g_padLast = 1;
        }
        g_padPlay = g_padLast && mode == MODE_GAME;
        g_hideCursor = c_hideCursor && g_padLast && mode == MODE_GAME;
    }
    if (mode == MODE_GAME) game_update(now, btn, pressed, lx, ly, rx, ry);
    else if (mode == MODE_MENU) menu_update(now, dt, btn, pressed, rx, ry, entered);
    else if (pressed & (B_A | B_B | B_START)) tap_key(VK_ESCAPE);

    g_prevButtons = btn;
    g_inTick = 0;
}

/* ------------------------------------------------------------------ hook */
static BOOL (WINAPI *g_origPeek)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI hk_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r)
{
    tick();
    return g_origPeek(m, h, a, b, r);
}

static int hook_iat(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        if (lstrcmpiA((char *)(base + imp->Name), "USER32.dll")) continue;
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)n->Name, "PeekMessageA")) continue;
            if ((void *)iat->u1.Function == (void *)hk_PeekMessageA) return 2; /* still in place */
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
            g_origPeek = (void *)iat->u1.Function;
            iat->u1.Function = (DWORD_PTR)hk_PeekMessageA;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* Second route onto the main thread: subclass the game window and run from a timer. */
static WNDPROC g_origWndProc;
static HWND g_subHwnd;
static int g_timerSet;
static LRESULT CALLBACK hk_WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_timerSet) { g_timerSet = 1; SetTimer(h, 0xD5AD, 8, NULL); }
    if (msg == WM_TIMER && w == 0xD5AD) { if (!g_shutdown) tick(); return 0; }
    if (msg == WM_CLOSE) logf_("window: close requested");
    if (msg == WM_DESTROY) { g_shutdown = 1; g_hideCursor = 0; logf_("window: destroyed, mod switched off"); }
    return CallWindowProcA(g_origWndProc, h, msg, w, l);
}
static BOOL CALLBACK find_wnd(HWND h, LPARAM out)
{
    DWORD pid = 0;
    RECT rc;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (!GetClientRect(h, &rc) || rc.right < 320 || rc.bottom < 200) return TRUE;
    *(HWND *)out = h;
    return FALSE;
}
static DWORD WINAPI watcher(LPVOID p)
{
    (void)p;
    int lastHook = -1;
    for (int i = 0;; i++) {
        Sleep(i < 40 ? 250 : 2000);
        int r = hook_iat();
        if (r != lastHook) { logf_("PeekMessageA import hook: %s", r == 0 ? "failed" : r == 1 ? "installed" : "in place"); lastHook = r; }
        if (g_subHwnd && !IsWindow(g_subHwnd)) { g_subHwnd = NULL; g_timerSet = 0; }
        if (!g_subHwnd) {
            HWND h = NULL;
            EnumWindows(find_wnd, (LPARAM)&h);
            if (h) {
                char cls[64] = "";
                GetClassNameA(h, cls, sizeof cls);
                g_origWndProc = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)hk_WndProc);
                if (g_origWndProc) {
                    g_subHwnd = h;
                    PostMessageA(h, WM_NULL, 0, 0);
                    logf_("subclassed game window %p (class %s)", (void *)h, cls);
                } else logf_("could not subclass window %p (class %s), error %lu", (void *)h, cls, GetLastError());
            }
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) { g_shutdown = 1; logf_("process exiting (%s)", reserved ? "normal" : "library unloaded"); return TRUE; }
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
#ifdef PROXY_VERSION
        proxy_init();
#endif
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char *s = strrchr(g_dir, '\\');
        if (s) s[1] = 0;
        snprintf(g_ini, sizeof g_ini, "%sdspad.ini", g_dir);
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        const char *b = strrchr(exe, '\\');
        logf_("---- DSPad build 70 loaded into %s (pid %lu)", exe, GetCurrentProcessId());
        char mark[8];
        if (GetEnvironmentVariableA("DSPAD_ACTIVE", mark, sizeof mark)) { logf_("another DSPad copy is already active in this process; this one stays idle"); return TRUE; }
        SetEnvironmentVariableA("DSPAD_ACTIVE", "1");
        if (!lstrcmpiA(b ? b + 1 : exe, "DSLOA.exe")) {
            int r = hook_iat();
            logf_("PeekMessageA import hook at load: %s", r ? "installed" : "failed");
            CloseHandle(CreateThread(NULL, 0, watcher, NULL, 0, NULL));
        }
    }
    return TRUE;
}
