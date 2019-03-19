#include "cbase.h"

#include "in_buttons.h"
#include "mom_timer.h"
#include "movevars_shared.h"
#include "mom_system_saveloc.h"
#include "mom_triggers.h"
#include "mom_player_shared.h"
#include "mom_replay_system.h"
#include <ctime>

#include "tier0/memdbgon.h"

class CTimeTriggerTraceEnum : public IEntityEnumerator
{
  public:
    CTimeTriggerTraceEnum(Ray_t *pRay, Vector velocity) : m_vecVelocity(velocity), m_pRay(pRay) { m_flOffset = 0.0f; }

    bool EnumEntity(IHandleEntity *pHandleEntity) OVERRIDE;
    float GetOffset() { return m_flOffset; }

  private:
    float m_flOffset;
    Vector m_vecVelocity;
    Ray_t *m_pRay;
};

CMomentumTimer::CMomentumTimer(const char *pName)
    : CAutoGameSystemPerFrame(pName), m_iZoneCount(0), m_iStartTick(0), m_iEndTick(0), m_iLastZone(0),
      m_iLastRunDate(0), m_bIsRunning(false), m_bWereCheatsActivated(false), m_bMapIsLinear(false),
      m_pStartTrigger(nullptr), m_pEndTrigger(nullptr), m_pCurrentZone(nullptr),
      m_pStartZoneMark(nullptr), m_iBonusZone(0), m_bShouldUseStartZoneOffset(false)
{
}

void CMomentumTimer::PostInit()
{
    g_pModuleComms->ListenForEvent("player_jump", UtlMakeDelegate(this, &CMomentumTimer::OnPlayerJump));
    g_pModuleComms->ListenForEvent("player_land", UtlMakeDelegate(this, &CMomentumTimer::OnPlayerLand));
}

void CMomentumTimer::LevelInitPostEntity()
{
    SetGameModeConVars();
    m_bWereCheatsActivated = false;
    ClearStartMark();
    DispatchMapInfo();
}

void CMomentumTimer::LevelShutdownPreEntity()
{
    if (IsRunning())
        Stop(false);
    m_bWereCheatsActivated = false;
    SetStartTrigger(nullptr);
    SetCurrentZone(nullptr);
    ClearStartMark();
}

void CMomentumTimer::FrameUpdatePreEntityThink()
{
    if (!GotCaughtCheating())
    {
        static ConVarRef sv_cheats("sv_cheats");
        if (sv_cheats.GetBool())
        {
            SetCheating(true);
            Stop();
        }
    }
}

bool CMomentumTimer::Start(int start, int iBonusZone)
{
    static ConVarRef mom_zone_edit("mom_zone_edit");

    CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());
    if (!pPlayer)
        return false;

    // Perform all the checks to ensure player can start
    // MOM_TODO: Display this info properly to client?
    if (g_pMOMSavelocSystem->IsUsingSaveLocMenu())
    {
        // MOM_TODO: Allow it based on gametype
        Warning("Cannot start timer while using save loc menu!\n");
        return false;
    }
    if (mom_zone_edit.GetBool())
    {
        Warning("Cannot start timer while editing zones!\n");
        return false;
    }
    if (pPlayer->m_bHasPracticeMode)
    {
        Warning("Cannot start timer while in practice mode!\n");
        return false;
    }
    if (pPlayer->GetMoveType() == MOVETYPE_NOCLIP)
    {
        Warning("Cannot start timer while in noclip!\n");
        return false;
    }

    m_iStartTick = start;
    m_iEndTick = 0;
    m_iLastRunDate = 0;
    SetRunning(true);

    // Dispatch a start timer message for the local player
    DispatchTimerEventMessage(pPlayer, TIMER_EVENT_STARTED);

    return true;
}

void CMomentumTimer::Stop(bool endTrigger /* = false */, bool stopRecording /* = true*/)
{
    SetRunning(false);
    g_ReplaySystem.SetPaused(false);

    CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());

    if (pPlayer)
    {
        // Set our end time and date
        if (endTrigger && !m_bWereCheatsActivated)
        {
            m_iEndTick = gpGlobals->tickcount;
            g_ReplaySystem.SetTimerStopTick(m_iEndTick);
            time(&m_iLastRunDate); // Set the last run date for the replay
        }

        DispatchTimerEventMessage(pPlayer, TIMER_EVENT_STOPPED);
        // Fire off the timer_state event
        if (timerStateEvent)
        {
            timerStateEvent->SetInt("ent", pPlayer->entindex());
            timerStateEvent->SetBool("is_running", false);
            gameeventmanager->FireEvent(timerStateEvent);
        }
    }

    // Stop replay recording, if there was any
    if (g_ReplaySystem.IsRecording() && stopRecording)
        g_ReplaySystem.StopRecording(!endTrigger || m_bWereCheatsActivated, endTrigger);
}

void CMomentumTimer::Reset()
{
    CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());

    g_pMOMSavelocSystem->SetUsingSavelocMenu(false); // It'll get set to true if they teleport to a CP out of here
    pPlayer->ResetRunStats();                        // Reset run stats
    pPlayer->m_Data.m_bMapFinished = false;
    pPlayer->m_Data.m_bTimerRunning = false;
    pPlayer->m_Data.m_flRunTime = 0.0f; // MOM_TODO: Do we want to reset this?

    if (g_pMomentumTimer->IsRunning())
    {
        g_pMomentumTimer->Stop(false, false); // Don't stop our replay just yet
        g_pMomentumTimer->DispatchResetMessage();
    }
    else
    {
        // Reset last jump velocity when we enter the start zone without a timer
        pPlayer->m_Data.m_flLastJumpVel = 0;

        // Handle the replay recordings
        if (g_ReplaySystem.IsRecording())
            g_ReplaySystem.StopRecording(true, false);

        g_ReplaySystem.BeginRecording();
    }
}

void CMomentumTimer::OnPlayerSpawn(CMomentumPlayer *pPlayer)
{
    DispatchNoZonesMsg();

    // MOM_TODO
    // If we do implement a gamemode interface this would be much better suited there
    static ConVarRef mom_gamemode("mom_gamemode");
    switch (mom_gamemode.GetInt())
    {
    case GAMEMODE_KZ:
        pPlayer->DisableAutoBhop();
        break;
    default:
        pPlayer->EnableAutoBhop();
        break;
    }

    DispatchTimerEventMessage(pPlayer, TIMER_EVENT_STOPPED);
}

void CMomentumTimer::OnPlayerJump(KeyValues *kv)
{
    CMomentumPlayer *pPlayer = static_cast<CMomentumPlayer*>(kv->GetPtr("player"));
    if (!pPlayer)
        return;

    // OnCheckBhop code
    pPlayer->m_bDidPlayerBhop = gpGlobals->tickcount - pPlayer->m_iLandTick < NUM_TICKS_TO_BHOP;
    if (!pPlayer->m_bDidPlayerBhop)
        pPlayer->m_iSuccessiveBhops = 0;

    pPlayer->m_Data.m_flLastJumpVel = pPlayer->GetLocalVelocity().Length2D();
    pPlayer->m_iSuccessiveBhops++;

    if (pPlayer->m_Data.m_bIsInZone && pPlayer->m_Data.m_iCurrentZone == 1 && pPlayer->m_bTimerStartOnJump)
    {
        TryStart(pPlayer, false);
    }

    // Set our runstats jump count
    if (IsRunning())
    {
        const int currentZone = pPlayer->m_Data.m_iCurrentZone;
        pPlayer->m_RunStats.SetZoneJumps(0, pPlayer->m_RunStats.GetZoneJumps(0) + 1);                     // Increment total jumps
        pPlayer->m_RunStats.SetZoneJumps(currentZone, pPlayer->m_RunStats.GetZoneJumps(currentZone) + 1); // Increment zone jumps
    }
}

void CMomentumTimer::OnPlayerLand(KeyValues *kv)
{
    CMomentumPlayer *pPlayer = static_cast<CMomentumPlayer*>(kv->GetPtr("player"));
    if (!pPlayer)
        return;

    if (pPlayer->m_Data.m_bIsInZone && pPlayer->m_Data.m_iCurrentZone == 1 && pPlayer->m_bTimerStartOnJump)
    {
        // Doesn't seem to work here, seems like it doesn't get applied to gamemovement's.
        // MOM_TODO: Check what's wrong.

        /*
        Vector vecNewVelocity = GetAbsVelocity();

        float flMaxSpeed = GetPlayerMaxSpeed();

        if (m_SrvData.m_bShouldLimitPlayerSpeed && vecNewVelocity.Length2D() > flMaxSpeed)
        {
            float zSaved = vecNewVelocity.z;

            VectorNormalizeFast(vecNewVelocity);

            vecNewVelocity *= flMaxSpeed;
            vecNewVelocity.z = zSaved;
            SetAbsVelocity(vecNewVelocity);
        }
        */

        // If we start timer on jump then we should reset on land
        Reset();
    }
}

void CMomentumTimer::OnPlayerEnterZone(CMomentumPlayer *pPlayer, CBaseMomZoneTrigger *pTrigger, int zonenum)
{
    const int zonetype = pTrigger->GetZoneType();
    if (zonetype == ZONE_TYPE_STOP)
    {
        // We've reached end zone, stop here
        auto pStopTrigger = static_cast<CTriggerTimerStop *>(pTrigger);
        SetEndTrigger(pStopTrigger);

        if (IsRunning() && !pPlayer->IsSpectatingGhost() &&
            pPlayer->m_Data.m_iBonusZone == pStopTrigger->GetZoneNumber() &&
            !pPlayer->m_bHasPracticeMode)
        {
            int zoneNum = pPlayer->m_Data.m_iCurrentZone;

            // This is needed so we have an ending velocity.

            const float endvel = pPlayer->GetLocalVelocity().Length();
            const float endvel2D = pPlayer->GetLocalVelocity().Length2D();

            pPlayer->m_RunStats.SetZoneExitSpeed(zoneNum, endvel, endvel2D);

            // Check to see if we should calculate the timer offset fix
            if (pStopTrigger->ContainsPosition(pPlayer->GetPreviousOrigin()))
                DevLog("PrevOrigin inside of end trigger, not calculating offset!\n");
            else
            {
                DevLog("Previous origin is NOT inside the trigger, calculating offset...\n");
                CalculateTickIntervalOffset(pPlayer, ZONE_TYPE_STOP);
            }

            // This is needed for the final stage
            pPlayer->m_RunStats.SetZoneTime(zoneNum, g_pMomentumTimer->GetCurrentTime() -
                                                         pPlayer->m_RunStats.GetZoneEnterTime(zoneNum));

            // Ending velocity checks

            float finalVel = endvel;
            float finalVel2D = endvel2D;

            if (endvel <= pPlayer->m_RunStats.GetZoneVelocityMax(0, false))
                finalVel = pPlayer->m_RunStats.GetZoneVelocityMax(0, false);

            if (endvel2D <= pPlayer->m_RunStats.GetZoneVelocityMax(0, true))
                finalVel2D = pPlayer->m_RunStats.GetZoneVelocityMax(0, true);

            pPlayer->m_RunStats.SetZoneVelocityMax(0, finalVel, finalVel2D);
            pPlayer->m_RunStats.SetZoneExitSpeed(0, endvel, endvel2D);

            // Stop the timer
            Stop(true);
            pPlayer->m_Data.m_flRunTime = GetLastRunTime();
            pPlayer->m_Data.m_iRunTimeTicks = m_iEndTick - m_iStartTick;
            // The map is now finished, show the mapfinished panel
            pPlayer->m_Data.m_bMapFinished = true;
            pPlayer->m_Data.m_bTimerRunning = false;
        }
    }
    else if (zonetype == ZONE_TYPE_STAGE || zonetype == ZONE_TYPE_START)
    {
        auto pStageTrigger = static_cast<CTriggerStage *>(pTrigger);
        SetCurrentZone(pStageTrigger);

        // Reset timer when we enter start zone
        if (zonetype == ZONE_TYPE_START)
        {
            SetStartTrigger(static_cast<CTriggerTimerStart *>(pTrigger));
            Reset();
        }
        else if (m_bIsRunning)
        {
            pPlayer->m_RunStats.SetZoneExitSpeed(zonenum - 1, pPlayer->GetLocalVelocity().Length(),
                                                 pPlayer->GetLocalVelocity().Length2D());
            CalculateTickIntervalOffset(pPlayer, ZONE_TYPE_STOP);
            const float fCurrentZoneEnterTime = CalculateStageTime(zonenum);
            pPlayer->m_RunStats.SetZoneEnterTime(zonenum, fCurrentZoneEnterTime);
            pPlayer->m_RunStats.SetZoneTime(zonenum - 1, fCurrentZoneEnterTime -
                                                         CalculateStageTime(zonenum - 1));
        }
    }
}

void CMomentumTimer::OnPlayerExitZone(CMomentumPlayer *pPlayer, CBaseMomZoneTrigger *pTrigger, int zonenum)
{
    if (pTrigger->GetZoneType() == ZONE_TYPE_START)
    {
        // This handles both the start and stage triggers
        CalculateTickIntervalOffset(pPlayer, ZONE_TYPE_START);

        TryStart(pPlayer, true);
    }

    if (pTrigger->GetZoneType() == ZONE_TYPE_START || pTrigger->GetZoneType() == ZONE_TYPE_STAGE)
    {
        // Timer won't be running if it's the start trigger
        if ((zonenum == 1 || IsRunning()) && !pPlayer->m_bHasPracticeMode)
        {
            float enterVel3D = pPlayer->GetLocalVelocity().Length(),
                  enterVel2D = pPlayer->GetLocalVelocity().Length2D();
            pPlayer->m_RunStats.SetZoneEnterSpeed(zonenum, enterVel3D, enterVel2D);
            if (zonenum == 1)
                pPlayer->m_RunStats.SetZoneEnterSpeed(0, enterVel3D, enterVel2D);
        }
    }
}

void CMomentumTimer::TryStart(CMomentumPlayer* pPlayer, bool bUseStartZoneOffset)
{
    // do not start timer if player is in practice mode or it's already running.
    if (!IsRunning())
    {
        SetShouldUseStartZoneOffset(bUseStartZoneOffset);

        // The Start method could fail if CP menu or prac mode is activated here
        if (Start(gpGlobals->tickcount, pPlayer->m_Data.m_iBonusZone))
        {
            // Used for trimming later on
            if (g_ReplaySystem.IsRecording())
            {
                g_ReplaySystem.SetTimerStartTick(gpGlobals->tickcount);
            }

            pPlayer->m_Data.m_bTimerRunning = true;
            // Used for spectating later on
            pPlayer->m_Data.m_iStartTick = gpGlobals->tickcount;

            // Are we in mid air when we started? If so, our first jump should be 1, not 0
            if (pPlayer->IsInAirDueToJump())
            {
                pPlayer->m_RunStats.SetZoneJumps(0, 1);
                pPlayer->m_RunStats.SetZoneJumps(pPlayer->m_Data.m_iCurrentZone, 1);
            }
        }
        else
        {
            DispatchTimerEventMessage(pPlayer, TIMER_EVENT_FAILED);
        }
    }
    else
    {
        SetShouldUseStartZoneOffset(!bUseStartZoneOffset);
    }

    pPlayer->m_Data.m_bMapFinished = false;
}

void CMomentumTimer::DispatchMapInfo()
{
    // Make sure zone count is up to date
    RequestZoneCount();

    IGameEvent *mapInitEvent = gameeventmanager->CreateEvent("map_init");
    if (mapInitEvent)
    {
        // MOM_TODO: for now it's assuming stages are on staged maps, load this from
        // either the RequestStageCount() method, or something else (map info file?)
        mapInitEvent->SetBool("is_linear", m_iZoneCount == 0);
        mapInitEvent->SetInt("num_zones", m_iZoneCount);
        IGameEvent *pCopy = gameeventmanager->DuplicateEvent(mapInitEvent);
        gameeventmanager->FireEvent(mapInitEvent);
        gameeventmanager->FireEventClientSide(pCopy);
    }
}

void CMomentumTimer::DispatchNoZonesMsg() const
{
    if (!GetZoneCount())
    {
        CSingleUserRecipientFilter filter(UTIL_GetLocalPlayer());
        filter.MakeReliable();
        UserMessageBegin(filter, "MB_NoStartOrEnd");
        MessageEnd();
    }
}

void CMomentumTimer::DispatchResetMessage() const
{
    CSingleUserRecipientFilter user(UTIL_GetLocalPlayer());
    user.MakeReliable();
    UserMessageBegin(user, "Timer_Reset");
    MessageEnd();
}

void CMomentumTimer::DispatchTimerEventMessage(CBasePlayer *pPlayer, int type) const
{
    IGameEvent *pEvent = gameeventmanager->CreateEvent("timer_event");
    if (pEvent)
    {
        pEvent->SetInt("ent", pPlayer->entindex());
        pEvent->SetInt("type", type);
        gameeventmanager->FireEvent(pEvent);
    }

    CSingleUserRecipientFilter user(pPlayer);
    user.MakeReliable();

    UserMessageBegin(user, "Timer_Event");
        WRITE_LONG(type);
    MessageEnd();
}

int CMomentumTimer::GetCurrentZoneNumber() const
{
    return m_pCurrentZone && m_pCurrentZone->GetZoneNumber();
}

static int GetNumEntitiesByClassname(const char* classname)
{
    int count = 0;

    CBaseEntity *pEnt = gEntList.FindEntityByClassname(nullptr, classname);
    while (pEnt)
    {
        count++;
        pEnt = gEntList.FindEntityByClassname(pEnt, classname);
    }

    return count;
}

void CMomentumTimer::RequestZoneCount()
{
    m_iZoneCount = GetNumEntitiesByClassname("trigger_momentum_timer_start") +
                   GetNumEntitiesByClassname("trigger_momentum_timer_stage") +
                   GetNumEntitiesByClassname("trigger_momentum_timer_checkpoint");
}

// This function is called every time CTriggerStage::StartTouch is called
float CMomentumTimer::CalculateStageTime(int stage)
{
    if (stage > m_iLastZone)
    {
        float originalTime = GetCurrentTime();
        // If the stage is a new one, we store the time we entered this stage in
        m_flZoneEnterTime[stage] = stage == 1 ? 0.0f : // Always returns 0 for first stage.
                                       originalTime + m_flTickOffsetFix[stage - 1];
        DevLog("Original Time: %f\n New Time: %f\n", originalTime, m_flZoneEnterTime[stage]);
    }
    m_iLastZone = stage;
    return m_flZoneEnterTime[stage];
}

float CMomentumTimer::GetLastRunTime()
{
    if (m_iEndTick == 0)
        return 0.0f;

    const float originalTime = static_cast<float>(m_iEndTick - m_iStartTick) * gpGlobals->interval_per_tick;
    // apply precision fix, adding offset from start as well as subtracting offset from end.
    // offset from end is 1 tick - fraction offset, since we started trace outside of the end zone.
    return originalTime;
    /*if (m_bShouldUseStartZoneOffset)
    {
        return originalTime + m_flTickOffsetFix[1] - (gpGlobals->interval_per_tick - m_flTickOffsetFix[0]);
    }
    else
    {
        return originalTime - (gpGlobals->interval_per_tick - m_flTickOffsetFix[0]);
    }*/
}

void CMomentumTimer::SetRunning(bool isRunning)
{
    m_bIsRunning = isRunning;
    CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());
    if (pPlayer)
    {
        pPlayer->m_Data.m_bTimerRunning = isRunning;
    }
}
void CMomentumTimer::CalculateTickIntervalOffset(CMomentumPlayer *pPlayer, const int zoneType)
{
    if (!pPlayer)
        return;

    Ray_t ray;
    Vector start, end, offset;

    // Since EndTouch is called after PostThink (which is where previous origins are stored) we need to go 1 more tick
    // in the previous data to get the real previous origin.
    if (zoneType == ZONE_TYPE_START) // EndTouch
    {
        start = pPlayer->GetLocalOrigin();
        end = pPlayer->GetPreviousOrigin(1);
    }
    else // StartTouch
    {
        start = pPlayer->GetPreviousOrigin();
        end = pPlayer->GetLocalOrigin();
    }

    ray.Init(start, end, pPlayer->CollisionProp()->OBBMins(), pPlayer->CollisionProp()->OBBMaxs());
    CTimeTriggerTraceEnum endTriggerTraceEnum(&ray, pPlayer->GetAbsVelocity());
    enginetrace->EnumerateEntities(ray, true, &endTriggerTraceEnum);

    DevLog("Time offset was %f seconds (%s)\n", endTriggerTraceEnum.GetOffset(),
           zoneType == ZONE_TYPE_START ? "EndTouch" : "StartTouch");
    SetIntervalOffset(GetCurrentZoneNumber(), endTriggerTraceEnum.GetOffset());
}

// override of IEntityEnumerator's EnumEntity() in order for our trace to hit zone triggers
bool CTimeTriggerTraceEnum::EnumEntity(IHandleEntity *pHandleEntity)
{
    trace_t tr;
    // store entity that we found on the trace
    CBaseEntity *pEnt = gEntList.GetBaseEntity(pHandleEntity->GetRefEHandle());

    // Stop the trace if this entity is solid.
    if (pEnt->IsSolid())
        return false;

    // if we aren't hitting a momentum trigger
    // the return type of EnumEntity tells the engine whether to continue enumerating future entities
    // or not.
    if (Q_strnicmp(pEnt->GetClassname(), "trigger_momentum_", Q_strlen("trigger_momentum_")) == 1)
        return false;

    // In this case, we want to continue in case we hit another type of trigger.

    enginetrace->ClipRayToEntity(*m_pRay, MASK_ALL, pHandleEntity, &tr);
    if (tr.fraction > 0.0f)
    {
        m_flOffset = tr.startpos.DistTo(tr.endpos) / m_vecVelocity.Length();

        // Account for slowmotion/timescale
        m_flOffset /= gpGlobals->interval_per_tick / gpGlobals->frametime;
        return true; // We hit our target
    }

    return false;
}

// set ConVars according to Gamemode. Tickrate is by in tickset.h
void CMomentumTimer::SetGameModeConVars()
{
    ConVarRef gm("mom_gamemode");
    switch (gm.GetInt())
    {
    case GAMEMODE_SURF:
        sv_maxvelocity.SetValue(3500);
        sv_airaccelerate.SetValue(150);
        sv_maxspeed.SetValue(260);
        break;
    case GAMEMODE_BHOP:
        sv_maxvelocity.SetValue(100000);
        sv_airaccelerate.SetValue(1000);
        sv_maxspeed.SetValue(260);
        break;
    case GAMEMODE_KZ:
        sv_maxvelocity.SetValue(3500);
        sv_airaccelerate.SetValue(100);
        sv_maxspeed.SetValue(250);
        break;
    case GAMEMODE_UNKNOWN:
        sv_maxvelocity.SetValue(3500);
        sv_airaccelerate.SetValue(150);
        sv_maxspeed.SetValue(260);
        break;
    default:
        DevWarning("[%i] GameMode not defined.\n", gm.GetInt());
        break;
    }
    DevMsg("CTimer set values:\nsv_maxvelocity: %i\nsv_airaccelerate: %i \nsv_maxspeed: %i\n", sv_maxvelocity.GetInt(),
           sv_airaccelerate.GetInt(), sv_maxspeed.GetInt());
}

void CMomentumTimer::CreateStartMark()
{
    CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());
    if (!pPlayer)
        return;

    if (m_pStartTrigger && m_pStartTrigger->IsTouching(pPlayer))
    {
        // Rid the previous one
        ClearStartMark();

        m_pStartZoneMark = g_pMOMSavelocSystem->CreateSaveloc();
        if (m_pStartZoneMark)
        {
            m_pStartZoneMark->vel = vec3_origin; // Rid the velocity
            DevLog("Successfully created a starting mark!\n");
        }
        else
        {
            Warning("Could not create the start mark for some reason!\n");
        }
    }
}

void CMomentumTimer::ClearStartMark()
{
    if (m_pStartZoneMark)
        delete m_pStartZoneMark;
    m_pStartZoneMark = nullptr;
}

// Practice mode that stops the timer and allows the player to noclip.
void CMomentumTimer::EnablePractice(CMomentumPlayer *pPlayer)
{
    pPlayer->SetParent(nullptr);
    pPlayer->SetMoveType(MOVETYPE_NOCLIP);
    ClientPrint(pPlayer, HUD_PRINTCONSOLE, "Practice mode ON!\n");
    pPlayer->AddEFlags(EFL_NOCLIP_ACTIVE);
    pPlayer->m_bHasPracticeMode = true;

    // Only when timer is running;
    if (m_bIsRunning)
    {
        pPlayer->m_Data.m_iOldBonusZone = pPlayer->m_Data.m_iBonusZone;
        pPlayer->m_Data.m_iOldZone = pPlayer->m_Data.m_iCurrentZone;
        pPlayer->m_vecLastPos = pPlayer->GetAbsOrigin();
        pPlayer->m_angLastAng = pPlayer->GetAbsAngles();
        pPlayer->m_vecLastVelocity = pPlayer->GetAbsVelocity();
        pPlayer->m_fLastViewOffset = pPlayer->GetViewOffset().z;

        // MOM_TODO: Mark this as a "entered practice mode" event in the replay
    }
    else
    {
        Stop(false); // Keep running
    }

    IGameEvent *pEvent = gameeventmanager->CreateEvent("practice_mode");
    if (pEvent)
    {
        pEvent->SetBool("enabled", true);
        gameeventmanager->FireEvent(pEvent);
    }
}

void CMomentumTimer::DisablePractice(CMomentumPlayer *pPlayer)
{
    pPlayer->RemoveEFlags(EFL_NOCLIP_ACTIVE);
    ClientPrint(pPlayer, HUD_PRINTCONSOLE, "Practice mode OFF!\n");
    pPlayer->SetMoveType(MOVETYPE_WALK);
    pPlayer->m_bHasPracticeMode = false;

    // Only when timer is running;
    if (m_bIsRunning)
    {
        pPlayer->m_Data.m_iBonusZone = pPlayer->m_Data.m_iOldBonusZone;
        pPlayer->m_Data.m_iCurrentZone = pPlayer->m_Data.m_iOldZone;
        pPlayer->Teleport(&pPlayer->m_vecLastPos, &pPlayer->m_angLastAng, &pPlayer->m_vecLastVelocity);
        pPlayer->SetViewOffset(Vector(0, 0, pPlayer->m_fLastViewOffset));
        pPlayer->SetLastEyeAngles(pPlayer->m_angLastAng);

        // MOM_TODO : Mark this as a "exited practice mode" event in the replay
    }

    IGameEvent *pEvent = gameeventmanager->CreateEvent("practice_mode");
    if (pEvent)
    {
        pEvent->SetBool("enabled", false);
        gameeventmanager->FireEvent(pEvent);
    }
}

//--------- Commands --------------------------------
static MAKE_TOGGLE_CONVAR(
    mom_practice_safeguard, "1", FCVAR_ARCHIVE | FCVAR_REPLICATED,
    "Toggles the safeguard for enabling practice mode (not pressing any movement keys to enable). 0 = OFF, 1 = ON.\n");

class CTimerCommands
{
  public:
    static void ResetToStart()
    {
        CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetCommandClient());
        if (!pPlayer || !pPlayer->AllowUserTeleports())
            return;
        CTriggerTimerStart *start = g_pMomentumTimer->GetStartTrigger();
        if (start)
        {
            SavedLocation_t *pStartMark = g_pMomentumTimer->GetStartMark();
            if (pStartMark)
            {
                pStartMark->Teleport(pPlayer);
            }
            else
            {
                // Don't set angles if still in start zone.
                QAngle ang = start->GetLookAngles();
                pPlayer->Teleport(&start->WorldSpaceCenter(), (start->HasLookAngles() ? &ang : nullptr), &vec3_origin);
            }
            pPlayer->ResetRunStats();
        }
        else
        {
            CBaseEntity *startPoint = pPlayer->EntSelectSpawnPoint();
            if (startPoint)
            {
                pPlayer->Teleport(&startPoint->GetAbsOrigin(), &startPoint->GetAbsAngles(), &vec3_origin);
                pPlayer->ResetRunStats();
            }
        }
    }

    static void ResetToCheckpoint()
    {
        CTriggerZone *pStage = g_pMomentumTimer->GetCurrentZone();
        CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetCommandClient());
        if (pStage && pPlayer && pPlayer->AllowUserTeleports())
        {
            // MOM_TODO do a trace downwards from the top of the trigger's center to touchable land, teleport the player there
            pPlayer->Teleport(&pStage->WorldSpaceCenter(), nullptr, &vec3_origin);
        }
    }

    static void PracticeMove()
    {
        CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());
        if (!pPlayer || !pPlayer->AllowUserTeleports() || pPlayer->IsSpectatingGhost())
            return;

        if (!pPlayer->m_bHasPracticeMode)
        {
            if (g_pMomentumTimer->IsRunning() && mom_practice_safeguard.GetBool())
            {
                bool safeGuard = (pPlayer->m_nButtons & (IN_FORWARD | IN_MOVELEFT | IN_MOVERIGHT | IN_BACK | IN_JUMP | IN_DUCK | IN_WALK)) != 0;
                if (safeGuard)
                {
                    Warning("You cannot enable practice mode while moving when the timer is running! Toggle this with \"mom_practice_safeguard\"!\n");
                    return;
                }
            }

            g_pMomentumTimer->EnablePractice(pPlayer);
        }
        else
        {
            g_pMomentumTimer->DisablePractice(pPlayer);
        }
    }

    static void MarkStart() { g_pMomentumTimer->CreateStartMark(); }

    static void ClearStart() { g_pMomentumTimer->ClearStartMark(); }

    static void TeleToStage(const CCommand &args)
    {
        CMomentumPlayer *pPlayer = ToCMOMPlayer(UTIL_GetLocalPlayer());
        const Vector *pVec = nullptr;
        const QAngle *pAng = nullptr;
        if (pPlayer && args.ArgC() >= 2)
        {
            if (!pPlayer->AllowUserTeleports())
                return;

            // We get the desried index from the command (Remember that for us, args are 1 indexed)
            int desiredIndex = Q_atoi(args[1]);
            if (desiredIndex == 1)
            {
                // Index 1 is the start. If the timer has a mark, we use it
                SavedLocation_t *startMark = g_pMomentumTimer->GetStartMark();
                if (startMark)
                {
                    pVec = &startMark->pos;
                    pAng = &startMark->ang;
                }
                else
                {
                    // If no mark was found, we teleport to the center of the first trigger_momentum_timer_start we find
                    CBaseEntity *pEnt = gEntList.FindEntityByClassname(nullptr, "trigger_momentum_timer_start");
                    if (pEnt)
                    {
                        pVec = &pEnt->GetAbsOrigin();
                    }
                }
            }
            else
            {
                // Every other index is probably a stage (What about < 1 indexes? Mappers are weird and do "weirder"
                // stuff so...)
                CTriggerStage *pStage = nullptr;

                while ((pStage = static_cast<CTriggerStage *>(
                            gEntList.FindEntityByClassname(pStage, "trigger_momentum_timer_stage"))) != nullptr)
                {
                    if (pStage && pStage->GetZoneNumber() == desiredIndex)
                    {
                        pVec = &pStage->GetAbsOrigin();
                        pAng = &pStage->GetAbsAngles();
                        break;
                    }
                }
            }

            // Teleport if we have a destination
            if (pVec)
            {
                // pAng can be null here, it's okay
                pPlayer->Teleport(pVec, pAng, &vec3_origin);
                // Untouch our triggers
                pPlayer->PhysicsCheckForEntityUntouch();
                // Stop *after* the teleport
                g_pMomentumTimer->Stop();
            }
            else
            {
                Warning("Could not teleport to stage %i! Perhaps it doesn't exist?\n", desiredIndex);
            }
        }
    }
};

static ConCommand mom_practice("mom_practice", CTimerCommands::PracticeMove,
                               "Toggle. Stops timer and allows player to fly around in noclip.\n"
                               "Only activates when player is not pressing any movement inputs.\n",
                               FCVAR_CLIENTCMD_CAN_EXECUTE);
static ConCommand
    mom_mark_start("mom_mark_start", CTimerCommands::MarkStart,
                   "Marks a starting point inside the start trigger for a more customized starting location.\n",
                   FCVAR_NONE);
static ConCommand mom_mark_start_clear("mom_mark_start_clear", CTimerCommands::ClearStart,
                                       "Clears the saved start location, if there is one.\n", FCVAR_NONE);
static ConCommand mom_reset_to_start("mom_restart", CTimerCommands::ResetToStart,
                                     "Restarts the player to the start trigger.\n",
                                     FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_SERVER_CAN_EXECUTE);
static ConCommand mom_reset_to_checkpoint("mom_reset", CTimerCommands::ResetToCheckpoint,
                                          "Teleports the player back to the start of the current stage.\n",
                                          FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_SERVER_CAN_EXECUTE);

static ConCommand mom_stage_tele("mom_stage_tele", CTimerCommands::TeleToStage,
                                 "Teleports the player to the desired stage. Stops the timer (Useful for mappers)\n",
                                 FCVAR_CLIENTCMD_CAN_EXECUTE | FCVAR_SERVER_CAN_EXECUTE);

static CMomentumTimer s_Timer("CMomentumTimer");
CMomentumTimer *g_pMomentumTimer = &s_Timer;
