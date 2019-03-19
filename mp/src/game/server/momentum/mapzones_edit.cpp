#include "cbase.h"
#include "mapzones_edit.h"

#include "mapzones.h"
#include "mapzones_build.h"
#include "mom_player.h"
#include "mom_triggers.h"
#include "mom_player_shared.h"
#include "mom_timer.h"

#include "tier0/memdbgon.h"

static CMomBaseZoneBuilder *GetZoneBuilderForMethod(int method);
static void OnZoneEditingToggled(IConVar *var, const char *pOldVal, float fOldVal);
static void OnZoningMethodChanged(IConVar *var, const char *pOldValue, float flOldValue);
static int GetZoneTypeToCreate();

static void VectorSnapToGrid(Vector &dest, float gridsize);
static float SnapToGrid(float fl, float gridsize);
static void DrawReticle(const Vector &pos, float retsize);

static MAKE_TOGGLE_CONVAR_C(mom_zone_edit, "0", FCVAR_CHEAT, "Toggle zone editing.\n", OnZoneEditingToggled);
static MAKE_TOGGLE_CONVAR(mom_zone_ignorewarning, "0", FCVAR_CHEAT, "Lets you create zones despite map already having start and end.\n");
static ConVar mom_zone_grid("mom_zone_grid", "8", FCVAR_CHEAT, "Set grid size. 0 to disable.", true, 0, false, 0);
static ConVar mom_zone_type("mom_zone_type", "auto", FCVAR_CHEAT,
                            "The zone type that will be created when using mom_zone_mark/create. 'auto' creates a "
                            "start zone unless one already exists, in which case an end zone is created.f\n");
static ConVar mom_zone_bonus("mom_zone_bonus", "0", FCVAR_CHEAT,
                             "Whether the zone that is created will be a bonus zone or not", true, 0, false, 0);
static ConVar mom_zone_start_limitspdmethod("mom_zone_start_limitspdmethod", "1", FCVAR_CHEAT,
                                            "0 = Take into account player z-velocity, 1 = Ignore z-velocity.\n", true,
                                            0, true, 1);
static ConVar mom_zone_stage_num("mom_zone_stage_num", "0", FCVAR_CHEAT,
                                 "Set stage number. Should start from 2. 0 to automatically find one.\n", true, 0,
                                 false, 0);
static ConVar mom_zone_start_maxbhopleavespeed("mom_zone_start_maxbhopleavespeed", "250", FCVAR_CHEAT,
                                               "Max leave speed if player bhopped. 0 to disable.\n", true, 0, false, 0);
// static ConVar mom_zone_cp_num( "mom_zone_cp_num", "0", FCVAR_CHEAT, "Checkpoint number. 0 to automatically find one."
// );
static ConVar mom_zone_debug("mom_zone_debug", "0", FCVAR_CHEAT);
static ConVar mom_zone_usenewmethod("mom_zone_usenewmethod", "0", FCVAR_CHEAT,
                                    "If 1, use a new point-based zoning method (by Mehis).\n", OnZoningMethodChanged);

CON_COMMAND_F(mom_zone_zoomin, "Decrease reticle maximum distance.\n", FCVAR_CHEAT)
{
    g_MomZoneEdit.DecreaseZoom(mom_zone_grid.GetFloat());
}

CON_COMMAND_F(mom_zone_zoomout, "Increase reticle maximum distance.\n", FCVAR_CHEAT)
{
    g_MomZoneEdit.IncreaseZoom(mom_zone_grid.GetFloat());
}

CON_COMMAND_F(mom_zone_delete, "Delete zone types. Accepts start/stop/stage or an entity index.\n", FCVAR_CHEAT)
{
    // MOM_TODO: Deleting a zone while a player is inside it causes some weird issues, need to investigate
    if (!mom_zone_edit.GetBool())
        return;

    if (args.ArgC() > 1)
    {
        DevMsg("Attempting to delete '%s'\n", args[1]);

        int entindex = atoi(args[1]);

        if (entindex != 0)
        {
            CBaseEntity *pEnt = CBaseEntity::Instance(INDEXENT(entindex));

            if (pEnt && g_MomZoneEdit.GetEntityZoneType(pEnt) != -1)
            {
                UTIL_Remove(pEnt);

                // Update zone count
                g_pMomentumTimer->DispatchMapInfo();
            }
        }
        else
        {
            char szDelete[64];
            if (ZoneTypeToClass(g_MomZoneEdit.ShortNameToZoneType(args[1]), szDelete, sizeof(szDelete)))
            {
                CBaseEntity *pEnt = gEntList.FindEntityByClassname(nullptr, szDelete);
                while (pEnt)
                {
                    UTIL_Remove(pEnt);
                    pEnt = gEntList.FindEntityByClassname(pEnt, szDelete);
                }

                // Update zone count
                g_pMomentumTimer->DispatchMapInfo();
            }
        }
    }
}

CON_COMMAND_F(mom_zone_edit_existing, "Edit an existing zone. Requires entity index.\n", FCVAR_CHEAT)
{
    if (!mom_zone_edit.GetBool())
        return;

    if (args.ArgC() > 1)
    {
        DevMsg("Attempting to edit '%s'\n", args[1]);

        const int entindex = Q_atoi(args[1]);

        if (entindex != 0)
        {
            CBaseEntity *pEnt = CBaseEntity::Instance(INDEXENT(entindex));

            if (pEnt && g_MomZoneEdit.GetEntityZoneType(pEnt) != -1)
            {
                auto pZone = static_cast<CBaseMomZoneTrigger *>(pEnt);
                g_MomZoneEdit.SetBuilder(CreateZoneBuilderFromExisting(pZone));
                UTIL_Remove(pEnt);
            }
            else
            {
                Warning("Invalid entity index: %s. Must be a valid zone trigger entity.\n", args[1]);
            }
        }
        else
        {
            Warning("Invalid entity index: %s. Must be a number greater than 0.\n", args[1]);
        }
    }
}

CON_COMMAND_F(mom_zone_start_setlook,
              "Sets start zone teleport look angles. Will take yaw in degrees or use your angles if no arguments given.\n",
              FCVAR_CHEAT)
{
    if (!mom_zone_edit.GetBool())
        return;

    CBasePlayer *pPlayer = UTIL_GetLocalPlayer();
    if (!pPlayer)
        return;

    float yaw;

    if (args.ArgC() > 1)
    {
        yaw = Q_atof(args[1]);
    }
    else
    {
        yaw = pPlayer->EyeAngles()[1];
    }

    CBaseEntity *pEnt = gEntList.FindEntityByClassname(nullptr, "trigger_momentum_timer_start");

    while (pEnt)
    {
        CTriggerTimerStart *pStart = static_cast<CTriggerTimerStart *>(pEnt);

        if (pStart)
        {
            pStart->SetHasLookAngles(true);
            pStart->SetLookAngles(QAngle(0, yaw, 0));

            DevMsg("Set start zone angles to: %.1f, %.1f, %.1f\n", pStart->GetLookAngles()[0],
                   pStart->GetLookAngles()[1], pStart->GetLookAngles()[2]);
        }

        pEnt = gEntList.FindEntityByClassname(pEnt, "trigger_momentum_timer_start");
    }
}

CON_COMMAND_F(mom_zone_mark, "Starts building a zone.\n", FCVAR_CHEAT)
{
    if (!mom_zone_edit.GetBool())
        return;

    g_MomZoneEdit.OnMark();
}

CON_COMMAND_F(mom_zone_cancel, "Cancel the building of the current zone.\n", FCVAR_CHEAT)
{
    if (!mom_zone_edit.GetBool())
        return;

    g_MomZoneEdit.OnCancel();
}

CON_COMMAND_F(mom_zone_back, "Go back a step when zone building.\n", FCVAR_CHEAT)
{
    if (!mom_zone_edit.GetBool())
        return;

    g_MomZoneEdit.OnRemove();
}

CON_COMMAND_F(mom_zone_create, "Create the zone.\n", FCVAR_CHEAT)
{
    if (!mom_zone_edit.GetBool())
        return;

    g_MomZoneEdit.OnCreate();
}

CON_COMMAND_F(mom_zone_info,
              "Sends info about the trigger that is being looked at (if one exists). Internal usage only.\n",
              FCVAR_HIDDEN)
{
    class CZoneTriggerTraceEnum : public IEntityEnumerator
    {
      public:
        CZoneTriggerTraceEnum()
        {
            m_pZone = nullptr;
        }

        // Return true to continue enumerating or false to stop
        virtual bool EnumEntity(IHandleEntity *pHandleEntity) OVERRIDE
        {
            CBaseEntity *pEnt = gEntList.GetBaseEntity(pHandleEntity->GetRefEHandle());

            // Stop the trace if this entity is solid.
            if (pEnt->IsSolid())
            {
                return false;
            }

            m_pZone = dynamic_cast<CBaseMomentumTrigger *>(pEnt);
            if (m_pZone != nullptr)
            {
                // Found our target, stop here
                return false;
            }
            
            // No dice, let's keep searching
            return true;
        }

        CBaseMomentumTrigger *GetZone() const { return m_pZone; }

      private:
        CBaseMomentumTrigger *m_pZone;
    };

    CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());
    if (!pPlayer)
    {
        return;
    }

    Vector start, end;
    pPlayer->EyePositionAndVectors(&start, &end, nullptr, nullptr);
    end = start + end * MAX_TRACE_LENGTH;

    Ray_t ray;
    ray.Init(start, end, pPlayer->CollisionProp()->OBBMins(), pPlayer->CollisionProp()->OBBMaxs());

    // Only EnumerateEntities can hit triggers, normal TraceRay can't
    CZoneTriggerTraceEnum zoneTriggerTraceEnum;
    enginetrace->EnumerateEntities(ray, true, &zoneTriggerTraceEnum); 
    CBaseMomentumTrigger *pZone = zoneTriggerTraceEnum.GetZone();
    int zoneidx = pZone ? pZone->entindex() : -1;
    int zonetype = pZone ? g_MomZoneEdit.GetEntityZoneType(pZone) : -1;

    CSingleUserRecipientFilter user(pPlayer);
    user.MakeReliable();

    UserMessageBegin(user, "ZoneInfo");
        WRITE_LONG(zoneidx);
        WRITE_LONG(zonetype);
    MessageEnd();
}

CMomZoneEdit::CMomZoneEdit() : CAutoGameSystemPerFrame("MomentumZoneBuilder")
{
    m_bEditing = false;

    m_flReticleDist = 1024.0f;

    m_pBuilder = nullptr;
}

CMomZoneEdit::~CMomZoneEdit() {}

void CMomZoneEdit::StopEditing()
{
    m_bEditing = false;

    mom_zone_edit.SetValue(0);

    SetBuilder(nullptr);
}

void CMomZoneEdit::OnMark()
{
    // Player wants to mark a point (ie. "next build step" in the old method)

    auto pPlayer = GetPlayerBuilder();
    if (!pPlayer)
        return;

    auto pBuilder = GetBuilder();

    Vector pos;
    GetCurrentBuildSpot(pPlayer, pos);

    if (pBuilder->IsDone())
        pBuilder->Reset();

    pBuilder->Add(pPlayer, pos);

    // Builder may want to create the zone NOW, instead of manually.
    if (pBuilder->CheckOnMark() && pBuilder->IsReady())
    {
        OnCreate();
    }
}

void CMomZoneEdit::OnCreate(int zonetype)
{
    // Player wants to create the zone.

    auto pPlayer = GetPlayerBuilder();
    Vector pos;
    GetCurrentBuildSpot(pPlayer, pos);

    auto pBuild = GetBuilder();
    if (!pBuild->BuildZone(pPlayer, &pos))
    {
        return;
    }

    int type = zonetype != -1 ? zonetype : GetZoneTypeToCreate();
    if (type == -1)
    {
        pBuild->Reset();
        // It's still -1, something's wrong
        Warning("Failed to create zone\n");
        return;
    }

    DevMsg("Creating entity...\n");

    auto pEnt = CreateZoneEntity(type);
    if (!pEnt)
    {
        pBuild->Reset();
        Warning("Couldn't create zone ent!\n");
        return;
    }

    pEnt->Spawn();

    pBuild->FinishZone(pEnt);
    pBuild->Reset();

    pEnt->Activate();

    SetZoneProps(pEnt);

    DevMsg("Created zone entity %i.\n", pEnt->entindex());

    // Update zone count
    g_pMomentumTimer->DispatchMapInfo();
}

void CMomZoneEdit::OnRemove()
{
    // Player wants to go back a step.
    auto pPlayer = GetPlayerBuilder();
    if (!pPlayer)
        return;

    Vector pos;
    GetCurrentBuildSpot(pPlayer, pos);

    GetBuilder()->Remove(pPlayer, pos);
}

void CMomZoneEdit::OnCancel()
{
    // Remove it completely
    GetBuilder()->Reset();
}

bool CMomZoneEdit::GetCurrentBuildSpot(CMomentumPlayer *pPlayer, Vector &vecPos)
{
    trace_t tr;
    Vector fwd;

    pPlayer->EyeVectors(&fwd);

    UTIL_TraceLine(pPlayer->EyePosition(), pPlayer->EyePosition() + fwd * m_flReticleDist, MASK_PLAYERSOLID, pPlayer,
                   COLLISION_GROUP_NONE, &tr);

    vecPos = tr.endpos;

    if (mom_zone_grid.GetInt() > 0)
        VectorSnapToGrid(vecPos, (float)mom_zone_grid.GetInt());

    return true;
}

CMomBaseZoneBuilder *CMomZoneEdit::GetBuilder()
{
    if (!m_pBuilder)
        SetBuilder(GetZoneBuilderForMethod(mom_zone_usenewmethod.GetInt()));

    return m_pBuilder;
}

void CMomZoneEdit::SetBuilder(CMomBaseZoneBuilder *pNewBuilder)
{
    if (m_pBuilder)
        delete m_pBuilder;
    m_pBuilder = pNewBuilder;
}

void CMomZoneEdit::ResetBuilder()
{
    SetBuilder(nullptr);
    GetBuilder();
}

CMomentumPlayer *CMomZoneEdit::GetPlayerBuilder() const
{
    return static_cast<CMomentumPlayer *>(UTIL_GetLocalPlayer());
}

void CMomZoneEdit::LevelInitPostEntity() { StopEditing(); }

void CMomZoneEdit::FrameUpdatePostEntityThink()
{
    if (!mom_zone_edit.GetBool())
    {
        if (m_bEditing)
            StopEditing();
        return;
    }

    if (!m_bEditing)
        m_bEditing = true;

    const auto pPlayer = GetPlayerBuilder();
    if (!pPlayer)
        return;

    Vector vecAim;
    if (!GetCurrentBuildSpot(pPlayer, vecAim))
        return;

    GetBuilder()->OnFrame(pPlayer, vecAim);

    DrawReticle(vecAim, 8.0f);
}

void VectorSnapToGrid(Vector &dest, float gridsize)
{
    dest.x = SnapToGrid(dest.x, gridsize);
    dest.y = SnapToGrid(dest.y, gridsize);
    // Don't snap z so that point can hit ground
}

float SnapToGrid(float fl, float gridsize)
{
    float closest;
    float dif;

    closest = fl - fmodf(fl, gridsize);

    dif = fl - closest;

    if (dif > (gridsize / 2.0f))
    {
        closest += gridsize;
    }
    else if (dif < (-gridsize / 2.0f))
    {
        closest -= gridsize;
    }

    return closest;
}

void DrawReticle(const Vector &pos, float retsize)
{
    Vector p1, p2, p3, p4, p5, p6;

    p1 = pos;
    p1.x = pos.x + retsize;

    p2 = pos;
    p2.x = pos.x - retsize;

    p3 = pos;
    p3.y = pos.y + retsize;

    p4 = pos;
    p4.y = pos.y - retsize;

    p5 = pos;
    p5.z = pos.z + retsize;

    p6 = pos;
    p6.z = pos.z - retsize;

    DebugDrawLine(p1, p2, 255, 0, 0, true, -1.0f);
    DebugDrawLine(p3, p4, 0, 255, 0, true, -1.0f);
    DebugDrawLine(p5, p6, 0, 0, 255, true, -1.0f);
}

CBaseMomZoneTrigger *CMomZoneEdit::CreateZoneEntity(int type)
{
    char szClass[64];
    if (!ZoneTypeToClass(type, szClass, sizeof(szClass)))
    {
        return nullptr;
    }

    auto pEnt = CreateEntityByName(szClass);
    auto pRet = dynamic_cast<CBaseMomZoneTrigger *>(pEnt);

    // Not a valid momentum trigger, delete it.
    if (!pRet && pEnt)
        UTIL_RemoveImmediate(pEnt);

    return pRet;
}

void CMomZoneEdit::SetZoneProps(CBaseMomZoneTrigger *pEnt)
{
    switch (pEnt->GetZoneType())
    {
    case ZONE_TYPE_START:
        {
            auto pStart = static_cast<CTriggerTimerStart *>(pEnt);
            // bhop speed limit
            if (mom_zone_start_maxbhopleavespeed.GetFloat() > 0.0)
            {
                pStart->SetMaxLeaveSpeed(mom_zone_start_maxbhopleavespeed.GetFloat());
                pStart->SetIsLimitingSpeed(true);
            }
            else
            {
                pStart->SetIsLimitingSpeed(false);
            }

            pStart->SetZoneNumber(1);
            // MOM_TODO pStart->SetTrackNumber()
        }
        break;
    case ZONE_TYPE_STOP:
        {
            auto pStop = static_cast<CTriggerTimerStop *>(pEnt);
            pStop->SetZoneNumber(0);
            // MOM_TODO pStop->SetTrackNumber
        }
        break;
    case ZONE_TYPE_STAGE:
        // MOM_TODO: case ZONE_TYPE_CHECKPOINT:
        {
            auto *pStage = static_cast<CTriggerStage *>(pEnt);
            if (mom_zone_stage_num.GetInt() > 0)
            {
                pStage->SetZoneNumber(mom_zone_stage_num.GetInt());
            }
            else
            {
                int higheststage = 1;

                CBaseEntity *pTemp = gEntList.FindEntityByClassname(nullptr, "trigger_momentum_timer_stage");
                while (pTemp)
                {
                    CTriggerStage *pTempStage = static_cast<CTriggerStage *>(pTemp);

                    if (pTempStage && pTempStage->GetZoneNumber() > higheststage)
                    {
                        higheststage = pTempStage->GetZoneNumber();
                    }

                    pTemp = gEntList.FindEntityByClassname(pTemp, "trigger_momentum_timer_stage");
                }

                pStage->SetZoneNumber(higheststage + 1);
            }
            // MOM_TODO ( pStage || pCheckpoint )->SetTrackNumber
        }
    default:
        break;
    }
}

int CMomZoneEdit::GetEntityZoneType(CBaseEntity *pEnt)
{
    CBaseMomZoneTrigger *pTrigger = dynamic_cast<CBaseMomZoneTrigger*>(pEnt);
    if (pTrigger)
        return pTrigger->GetZoneType();

    return -1;
}

int CMomZoneEdit::ShortNameToZoneType(const char *in)
{
    if (Q_stricmp(in, "start") == 0)
    {
        return ZONE_TYPE_START;
    }
    else if (Q_stricmp(in, "end") == 0 || Q_stricmp(in, "stop") == 0)
    {
        return ZONE_TYPE_STOP;
    }
    else if (Q_stricmp(in, "stage") == 0)
    {
        return ZONE_TYPE_STAGE;
    }
    else if (Q_stricmp(in, "cp") == 0 || Q_stricmp(in, "checkpoint") == 0)
    {
        return ZONE_TYPE_CHECKPOINT;
    }

    return -1;
}

static CMomBaseZoneBuilder *GetZoneBuilderForMethod(int method)
{
    switch (mom_zone_usenewmethod.GetInt())
    {
    case 0:
        return new CMomBoxZoneBuilder;
    case 1:
        return new CMomPointZoneBuilder;
    default:
        // default to box zone
        return new CMomBoxZoneBuilder;
    }
}

static void OnZoningMethodChanged(IConVar *var, const char *pOldValue, float flOldValue)
{
    g_MomZoneEdit.ResetBuilder();
}

void OnZoneEditingToggled(IConVar* var, const char* pOldVal, float fOldVal)
{
    ConVarRef varRef(var);
    if (!varRef.GetBool())
    {
        g_MomZoneEdit.OnCancel();
    }
}

static int GetZoneTypeToCreate()
{
    int zonetype = g_MomZoneEdit.ShortNameToZoneType(mom_zone_type.GetString());
    bool bAutoCreate = false;
    if (zonetype == -1 && Q_stricmp(mom_zone_type.GetString(), "auto") == 0)
    {
        zonetype = ZONE_TYPE_START;
        bAutoCreate = true;
    }

    if (zonetype == ZONE_TYPE_START || zonetype == ZONE_TYPE_STOP)
    {
        // Count zones to make sure we don't create multiple instances.
        int startnum = 0;
        int endnum = 0;

        CBaseEntity *pEnt;

        pEnt = gEntList.FindEntityByClassname(nullptr, "trigger_momentum_timer_start");
        while (pEnt)
        {
            auto pTrigger = static_cast<CTriggerTimerStart *>(pEnt);
            if (pTrigger->GetZoneNumber() == mom_zone_bonus.GetInt())
            {
                startnum++;
            }

            pEnt = gEntList.FindEntityByClassname(pEnt, "trigger_momentum_timer_start");
        }

        pEnt = gEntList.FindEntityByClassname(nullptr, "trigger_momentum_timer_stop");
        while (pEnt)
        {
            auto pTrigger = static_cast<CTriggerTimerStop *>(pEnt);
            if (pTrigger->GetZoneNumber() == mom_zone_bonus.GetInt())
            {
                endnum++;
            }

            pEnt = gEntList.FindEntityByClassname(pEnt, "trigger_momentum_timer_stop");
        }

        DevMsg("Found %i starts and %i ends for bonus %i (previous)\n", startnum, endnum, mom_zone_bonus.GetInt());

        if (!mom_zone_ignorewarning.GetBool() && startnum && endnum)
        {
            // g_MapzoneEdit.SetBuildStage(BUILDSTAGE_NONE);

            ConMsg("Map already has a start and an end for this track! Use mom_zone_type to set another zone type.\n");

            return -1;
        }

        if (bAutoCreate)
        {
            // Switch between start and end.
            zonetype = (startnum <= endnum) ? ZONE_TYPE_START : ZONE_TYPE_STOP;
        }
        // else the zonetype can be STOP, allowing for multiple stop triggers to be created
    }

    return zonetype;
}


// Expose to DLL
CMomZoneEdit g_MomZoneEdit;