#include "cbase.h"

#include "hud_mapfinished.h"
#include <vgui_controls/Label.h>
#include <game/client/iviewport.h>
#include "spectate/momSpectatorGUI.h"
#include "clientmode.h"
#include "mom_player_shared.h"

#include <vgui_controls/ImagePanel.h>
#include "vgui_controls/Tooltip.h"
#include <vgui/IInput.h>
#include "vgui/ISurface.h"
#include "vgui/ILocalize.h"

#include "mom_event_listener.h"
#include "util/mom_util.h"
#include "c_mom_replay_entity.h"

#include "tier0/memdbgon.h"

using namespace vgui;

DECLARE_HUDELEMENT_DEPTH(CHudMapFinishedDialog, 70);

//NOTE: The "CHudMapFinishedDialog" (main panel) control settings are found in MapFinishedDialog.res
CHudMapFinishedDialog::CHudMapFinishedDialog(const char *pElementName) : 
CHudElement(pElementName), BaseClass(g_pClientMode->GetViewport(), "CHudMapFinishedDialog"), m_pPlayer(nullptr)
{
    SetHiddenBits(HIDEHUD_WEAPONSELECTION);
    SetProportional(true);
    SetSize(10, 10); // Fix "not sized yet" spew
    m_pRunStats = nullptr;
    m_bIsGhost = false;
    m_iCurrentPage = 0;
    m_iMaxPageTitleWidth = 0;

    ListenForGameEvent("timer_state");
    ListenForGameEvent("replay_save");
    ListenForGameEvent("run_upload");

    surface()->CreatePopup(GetVPanel(), false, false, false, false, false);
    
    m_pClosePanelButton = new ImagePanel(this, "Close_Panel");
    m_pNextZoneButton = new ImagePanel(this, "Next_Zone");
    m_pPrevZoneButton = new ImagePanel(this, "Prev_Zone");
    m_pPlayReplayButton = new ImagePanel(this, "Replay_Icon");
    m_pRepeatButton = new ImagePanel(this, "Repeat_Button");
    m_pDetachMouseLabel = new Label(this, "Detach_Mouse", "#MOM_MF_DetachMouse");
    m_pCurrentZoneLabel = new Label(this, "Current_Zone", "#MOM_MF_OverallStats");
    m_pZoneOverallTime = new Label(this, "Zone_Overall_Time", "#MOM_MF_RunTime");
    m_pZoneEnterTime = new Label(this, "Zone_Enter_Time", "#MOM_MF_Zone_Enter");
    m_pZoneJumps = new Label(this, "Zone_Jumps", "#MOM_MF_Jumps");
    m_pZoneStrafes = new Label(this, "Zone_Strafes", "#MOM_MF_Strafes");
    m_pZoneVelEnter = new Label(this, "Zone_Vel_Enter", "#MOM_MF_Velocity_Enter");
    m_pZoneVelExit = new Label(this, "Zone_Vel_Exit", "#MOM_MF_Velocity_Exit");
    m_pZoneVelAvg = new Label(this, "Zone_Vel_Avg", "#MOM_MF_Velocity_Avg");
    m_pZoneVelMax = new Label(this, "Zone_Vel_Max", "#MOM_MF_Velocity_Max");
    m_pZoneSync1 = new Label(this, "Zone_Sync1", "#MOM_MF_Sync1");
    m_pZoneSync2 = new Label(this, "Zone_Sync2", "#MOM_MF_Sync2");
    m_pRunSaveStatus = new Label(this, "Run_Save_Status", "#MOM_MF_RunNotSaved");
    m_pRunUploadStatus = new Label(this, "Run_Upload_Status", "#MOM_MF_RunNotUploaded");

    LoadControlSettings("resource/ui/MapFinishedDialog.res");

    m_pNextZoneButton->SetMouseInputEnabled(true);
    m_pNextZoneButton->InstallMouseHandler(this);
    m_pPrevZoneButton->SetMouseInputEnabled(true);
    m_pPrevZoneButton->InstallMouseHandler(this);
    m_pPlayReplayButton->SetMouseInputEnabled(true);
    m_pPlayReplayButton->InstallMouseHandler(this);
    m_pRepeatButton->SetMouseInputEnabled(true);
    m_pRepeatButton->InstallMouseHandler(this);
    m_pClosePanelButton->SetMouseInputEnabled(true);
    m_pClosePanelButton->InstallMouseHandler(this);
    m_iCurrentZoneOrigX = m_pCurrentZoneLabel->GetXPos();

    SetPaintBackgroundEnabled(true);
    SetPaintBackgroundType(2);
    SetKeyBoardInputEnabled(false);
    SetMouseInputEnabled(false);
}

CHudMapFinishedDialog::~CHudMapFinishedDialog()
{
    m_pRunStats = nullptr;
}

void CHudMapFinishedDialog::FireGameEvent(IGameEvent* pEvent)
{
    if (!Q_strcmp(pEvent->GetName(), "timer_state"))
    {
        //We only care when this is false
        if (!pEvent->GetBool("is_running", true))
        {
            if (m_pPlayer)
            {
                ConVarRef hvel("mom_hud_speedometer_hvel");
                m_iVelocityType = hvel.GetBool();

                C_MomentumReplayGhostEntity *pGhost = m_pPlayer->GetReplayEnt();
                if (pGhost)
                {
                    m_pRunStats = &pGhost->m_RunStats;
                    m_pRunData = &pGhost->m_SrvData.m_RunData;
                    m_bIsGhost = true;
                }
                else
                {
                    m_pRunStats = &m_pPlayer->m_RunStats;
                    m_pRunData = &m_pPlayer->m_SrvData.m_RunData;
                    m_bIsGhost = false;
                }

                m_pPlayReplayButton->SetVisible(!m_bIsGhost);
                m_pRunUploadStatus->SetVisible(!m_bIsGhost);
                m_pRunSaveStatus->SetVisible(!m_bIsGhost);
                m_pRepeatButton->GetTooltip()->SetText(m_bIsGhost ? m_pszRepeatToolTipReplay : m_pszRepeatToolTipMap);

                CMOMSpectatorGUI *pPanel = dynamic_cast<CMOMSpectatorGUI*>(gViewPortInterface->FindPanelByName(PANEL_SPECGUI));
                if (pPanel && pPanel->IsVisible())
                    SetMouseInputEnabled(pPanel->IsMouseInputEnabled());
            }
        }
    }
    else if (FStrEq(pEvent->GetName(), "replay_save"))
    {
        m_bRunSaved = pEvent->GetBool("save");
        // MOM_TODO: There's a file name parameter as well, do we want to use it here?
    }
    else if (FStrEq(pEvent->GetName(), "run_upload"))
    {
        m_bRunUploaded = pEvent->GetBool("run_posted");
    }
}

void CHudMapFinishedDialog::LevelInitPostEntity()
{
    m_pPlayer = ToCMOMPlayer(CBasePlayer::GetLocalPlayer());
}

void CHudMapFinishedDialog::LevelShutdown()
{
    m_pPlayer = nullptr;
}

void CHudMapFinishedDialog::SetMouseInputEnabled(bool state)
{
    BaseClass::SetMouseInputEnabled(state);
    m_pDetachMouseLabel->SetVisible(!state);
}

bool CHudMapFinishedDialog::ShouldDraw()
{
    bool shouldDrawLocal = false;
    if (m_pPlayer)
    {
        C_MomentumReplayGhostEntity *pGhost = m_pPlayer->GetReplayEnt();
        CMOMRunEntityData *pData = (pGhost ? &pGhost->m_SrvData.m_RunData : &m_pPlayer->m_SrvData.m_RunData);
        shouldDrawLocal = pData && pData->m_bMapFinished;
    }

    if (!shouldDrawLocal)
        SetMouseInputEnabled(false);

    return CHudElement::ShouldDraw() && shouldDrawLocal && m_pRunStats;
}

void CHudMapFinishedDialog::ApplySchemeSettings(IScheme *pScheme)
{
    BaseClass::ApplySchemeSettings(pScheme);
    SetBgColor(GetSchemeColor("MOM.Panel.Bg", pScheme));
    m_pDetachMouseLabel->SetFont(m_hTextFont);
    m_pCurrentZoneLabel->SetFont(m_hTextFont);
    m_pZoneOverallTime->SetFont(m_hTextFont);
    m_pZoneEnterTime->SetFont(m_hTextFont);
    m_pZoneJumps->SetFont(m_hTextFont);
    m_pZoneStrafes->SetFont(m_hTextFont);
    m_pZoneVelEnter->SetFont(m_hTextFont);
    m_pZoneVelExit->SetFont(m_hTextFont);
    m_pZoneVelAvg->SetFont(m_hTextFont);
    m_pZoneVelMax->SetFont(m_hTextFont);
    m_pZoneSync1->SetFont(m_hTextFont);
    m_pZoneSync2->SetFont(m_hTextFont);
    m_pRunSaveStatus->SetFont(m_hTextFont);
    m_pRunUploadStatus->SetFont(m_hTextFont);
}

inline void FireMapFinishedClosedEvent(bool restart)
{
    IGameEvent *pClosePanel = gameeventmanager->CreateEvent("mapfinished_panel_closed");
    if (pClosePanel)
    {
        pClosePanel->SetBool("restart", restart);
        //Fire this event so other classes can get at this
        gameeventmanager->FireEvent(pClosePanel);
    }
}

void CHudMapFinishedDialog::OnMousePressed(MouseCode code)
{
    if (code == MOUSE_LEFT)
    {
        VPANEL over = input()->GetMouseOver();
        if (over == m_pPlayReplayButton->GetVPanel())
        {
            SetMouseInputEnabled(false);
            engine->ServerCmd("mom_replay_play_loaded");
            m_bRunSaved = false;
            m_bRunUploaded = false;
        }
        else if (over == m_pNextZoneButton->GetVPanel())
        {
            //MOM_TODO (beta+): Play animations?
            m_iCurrentPage = (m_iCurrentPage + 1) % (g_MOMEventListener->m_iMapZoneCount + 1);//;
        }
        else if (over == m_pPrevZoneButton->GetVPanel())
        {
            //MOM_TODO: (beta+) play animations?
            int newPage = m_iCurrentPage - 1;
            m_iCurrentPage = newPage < 0 ? g_MOMEventListener->m_iMapZoneCount : newPage;
        }
        else if (over == m_pRepeatButton->GetVPanel())
        {
            SetMouseInputEnabled(false);
            //The player either wants to repeat the replay (if spectating), or restart the map (not spec)
            engine->ServerCmd(m_bIsGhost ? "mom_replay_restart" : "mom_restart");
            FireMapFinishedClosedEvent(true);
            m_bRunSaved = false;
            m_bRunUploaded = false;
        }
        else if (over == m_pClosePanelButton->GetVPanel())
        {
            //This is where we unload comparisons, as well as the ghost if the player was speccing it
            SetMouseInputEnabled(false);
            FireMapFinishedClosedEvent(false);
            m_bRunSaved = false;
            m_bRunUploaded = false;
        }
    }
}


void CHudMapFinishedDialog::Init()
{
    Reset();
    // --- cache localization tokens ---
    //Label Tooltips
    LOCALIZE_TOKEN(repeatToolTipMap, "#MOM_MF_Restart_Map", m_pszRepeatToolTipMap);
    LOCALIZE_TOKEN(repeatToolTipReplay, "#MOM_MF_Restart_Replay", m_pszRepeatToolTipReplay);
    LOCALIZE_TOKEN(playReplatTooltip, "#MOM_MF_PlayReplay", m_pszPlayReplayToolTip);
    m_pPlayReplayButton->GetTooltip()->SetText(m_pszPlayReplayToolTip);
    LOCALIZE_TOKEN(rightArrowTT, "#MOM_MF_Right_Arrow", m_pszRightArrowToolTip);
    m_pNextZoneButton->GetTooltip()->SetText(m_pszRightArrowToolTip);
    LOCALIZE_TOKEN(leftTokenTT, "#MOM_MF_Left_Arrow", m_pszLeftArrowToolTip);
    m_pPrevZoneButton->GetTooltip()->SetText(m_pszLeftArrowToolTip);
    
    //Run saving/uploading
    FIND_LOCALIZATION(m_pwRunSavedLabel, "#MOM_MF_RunSaved");
    FIND_LOCALIZATION(m_pwRunNotSavedLabel, "#MOM_MF_RunNotSaved");
    FIND_LOCALIZATION(m_pwRunUploadedLabel, "#MOM_MF_RunUploaded");
    FIND_LOCALIZATION(m_pwRunNotUploadedLabel, "#MOM_MF_RunNotUploaded");

    // Stats
    FIND_LOCALIZATION(m_pwCurrentPageOverall, "#MOM_MF_OverallStats");
    FIND_LOCALIZATION(m_pwCurrentPageZoneNum, "#MOM_MF_ZoneNum");
    FIND_LOCALIZATION(m_pwOverallTime, "#MOM_MF_RunTime");
    FIND_LOCALIZATION(m_pwZoneEnterTime, "#MOM_MF_Zone_Enter");
    FIND_LOCALIZATION(m_pwZoneTime, "#MOM_MF_Time_Zone");
    FIND_LOCALIZATION(m_pwVelAvg, "#MOM_MF_Velocity_Avg");
    FIND_LOCALIZATION(m_pwVelMax, "#MOM_MF_Velocity_Max");
    FIND_LOCALIZATION(m_pwVelZoneEnter, "#MOM_MF_Velocity_Enter");
    FIND_LOCALIZATION(m_pwVelZoneExit, "#MOM_MF_Velocity_Exit");
    FIND_LOCALIZATION(m_pwJumpsOverall, "#MOM_MF_JumpCount");
    FIND_LOCALIZATION(m_pwJumpsZone, "#MOM_MF_Jumps");
    FIND_LOCALIZATION(m_pwStrafesOverall, "#MOM_MF_StrafeCount");
    FIND_LOCALIZATION(m_pwStrafesZone, "#MOM_MF_Strafes");
    FIND_LOCALIZATION(m_pwSync1Overall, "#MOM_MF_AvgSync");
    FIND_LOCALIZATION(m_pwSync1Zone, "#MOM_MF_Sync1");
    FIND_LOCALIZATION(m_pwSync2Overall, "#MOM_MF_AvgSync2");
    FIND_LOCALIZATION(m_pwSync2Zone, "#MOM_MF_Sync2");
}

void CHudMapFinishedDialog::Reset()
{
    //default values
    m_pRunStats = nullptr;
    strcpy(m_pszEndRunTime, "00:00:00.000");
}

void CHudMapFinishedDialog::SetVisible(bool b)
{
    BaseClass::SetVisible(b);
    //We reset the page to 0 when this this panel is shown because Reset() is not always called.
    if (b) 
        m_iCurrentPage = 0;
}

#define MAKE_UNI_NUM(name, size, number, format) \
    wchar_t name[size]; \
    V_snwprintf(name, size, format, number)

inline void PaintLabel(Label *label, wchar_t *wFormat, float value, bool isInt)
{
    wchar_t temp[BUFSIZELOCL], tempNum[BUFSIZESHORT];
    if (isInt)
    {
        int intVal = static_cast<int>(value);
        V_snwprintf(tempNum, BUFSIZESHORT, L"%i", intVal);
    }
    else
    {
        V_snwprintf(tempNum, BUFSIZESHORT, L"%.4f", value);
    }
    g_pVGuiLocalize->ConstructString(temp, sizeof(temp), wFormat, 1, tempNum);
    label->SetText(temp);
}

void CHudMapFinishedDialog::Paint()
{
    //text color
    surface()->DrawSetTextFont(m_hTextFont);
    surface()->DrawSetTextColor(GetFgColor());

    // --- CURRENT PAGE TITLE (ZONE) ---
    wchar_t currentPageTitle[BUFSIZELOCL];
    if (m_iCurrentPage == 0)
    {
        V_wcscpy_safe(currentPageTitle, m_pwCurrentPageOverall);
    }
    else
    {
        MAKE_UNI_NUM(num, 3, m_iCurrentPage, L"%i");
        g_pVGuiLocalize->ConstructString(currentPageTitle, sizeof(currentPageTitle), m_pwCurrentPageZoneNum, 1, num);
    }
    
    m_pCurrentZoneLabel->SetText(currentPageTitle);

    //// --- RUN TIME ---
    wchar_t currentZoneOverall[BUFSIZELOCL];
    wchar_t unicodeTime[BUFSIZETIME];
    //"Time:" shows up when m_iCurrentPage  == 0
    if (m_iCurrentPage < 1)// == 0, but I'm lazy to do an else-if
    {
        g_pMomentumUtil->FormatTime(m_pRunData ? m_pRunData->m_flRunTime : 0.0f, m_pszEndRunTime);
        ANSI_TO_UNICODE(m_pszEndRunTime, unicodeTime);
        g_pVGuiLocalize->ConstructString(currentZoneOverall, sizeof(currentZoneOverall), m_pwOverallTime, 1, unicodeTime);

        m_pZoneOverallTime->SetText(currentZoneOverall);//"Time" (overall run time)

        m_pZoneEnterTime->SetVisible(false);
        m_pZoneEnterTime->SetEnabled(false);
    }
    else
    {
        //"Zone Time:" shows up when m_iCurrentPage > 0
        char ansiTime[BUFSIZETIME];
        g_pMomentumUtil->FormatTime(m_pRunStats ? m_pRunStats->GetZoneTime(m_iCurrentPage) : 0.0f, ansiTime);
        ANSI_TO_UNICODE(ansiTime, unicodeTime);
        g_pVGuiLocalize->ConstructString(currentZoneOverall, sizeof(currentZoneOverall), m_pwZoneTime, 1, unicodeTime);
        m_pZoneOverallTime->SetText(currentZoneOverall);//"Zone time" (time for that zone)


        //"Zone Enter Time:" shows up when m_iCurrentPage > 1
        if (m_iCurrentPage > 1)
        {
            m_pZoneEnterTime->SetEnabled(true);
            m_pZoneEnterTime->SetVisible(true);
            wchar_t zoneEnterTime[BUFSIZELOCL];
            g_pMomentumUtil->FormatTime(m_pRunStats ? m_pRunStats->GetZoneEnterTime(m_iCurrentPage) : 0.0f, ansiTime);
            ANSI_TO_UNICODE(ansiTime, unicodeTime);
            g_pVGuiLocalize->ConstructString(zoneEnterTime, sizeof(zoneEnterTime), m_pwZoneEnterTime, 1, unicodeTime);
            m_pZoneEnterTime->SetText(zoneEnterTime);//"Zone enter time:" (time entered that zone)
        }
        else
        {
            m_pZoneEnterTime->SetVisible(false);
            m_pZoneEnterTime->SetEnabled(false);
        }
    }
    //// ---------------------

    //MOM_TODO: Set every label's Y pos higher if there's no ZoneEnterTime visible

    //// --- JUMP COUNT ---
    PaintLabel(m_pZoneJumps, 
        m_iCurrentPage == 0 ? m_pwJumpsOverall : m_pwJumpsZone,
        m_pRunStats ? m_pRunStats->GetZoneJumps(m_iCurrentPage) : 0, 
        true);
    //// ---------------------

    //// --- STRAFE COUNT ---
    PaintLabel(m_pZoneStrafes,
        m_iCurrentPage == 0 ? m_pwStrafesOverall : m_pwStrafesZone,
        m_pRunStats ? m_pRunStats->GetZoneStrafes(m_iCurrentPage) : 0,
        true);
    //// ---------------------

    //// --- SYNC1 ---
    PaintLabel(m_pZoneSync1,
        m_iCurrentPage == 0 ? m_pwSync1Overall : m_pwSync1Zone,
        m_pRunStats ? m_pRunStats->GetZoneStrafeSyncAvg(m_iCurrentPage) : 0.0f,
        false);
    //// ---------------------

    //// --- SYNC2---
    PaintLabel(m_pZoneSync2,
        m_iCurrentPage == 0 ? m_pwSync2Overall : m_pwSync2Zone,
        m_pRunStats ? m_pRunStats->GetZoneStrafeSync2Avg(m_iCurrentPage) : 0.0f,
        false);
    //// ---------------------

    //// --- STARTING VELOCITY---
    PaintLabel(m_pZoneVelEnter,
        m_pwVelZoneEnter,
        m_pRunStats ? m_pRunStats->GetZoneEnterSpeed(m_iCurrentPage, m_iVelocityType) : 0.0f,
        false);
    //// ---------------------

    //// --- ENDING VELOCITY---
    PaintLabel(m_pZoneVelExit,
        m_pwVelZoneExit,
        m_pRunStats ? m_pRunStats->GetZoneExitSpeed(m_iCurrentPage, m_iVelocityType) : 0.0f,
        false);
    //// ---------------------

    //// --- AVG VELOCITY---
    PaintLabel(m_pZoneVelAvg,
        m_pwVelAvg,
        m_pRunStats ? m_pRunStats->GetZoneVelocityAvg(m_iCurrentPage, m_iVelocityType) : 0.0f,
        false);
    //// ---------------------

    //// --- MAX VELOCITY---
    PaintLabel(m_pZoneVelMax,
        m_pwVelMax,
        m_pRunStats ? m_pRunStats->GetZoneVelocityMax(m_iCurrentPage, m_iVelocityType) : 0.0f,
        false);
    //// ---------------------

    //// ---- RUN SAVING AND UPLOADING ----

    //// -- run save --
    m_pRunSaveStatus->SetText(m_bRunSaved ? m_pwRunSavedLabel : m_pwRunNotSavedLabel);
    m_pRunSaveStatus->SetFgColor(m_bRunSaved ? COLOR_GREEN : COLOR_RED);

    //// -- run upload --
    //MOM_TODO: Should we have custom error messages here? One for server not responding, one for failed accept, etc
    m_pRunUploadStatus->SetText(m_bRunUploaded ? m_pwRunUploadedLabel : m_pwRunNotUploadedLabel);
    m_pRunUploadStatus->SetFgColor(m_bRunUploaded ? COLOR_GREEN : COLOR_RED);
    // ----------------
    // ------------------------------
}