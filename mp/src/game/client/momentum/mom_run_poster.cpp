#include "cbase.h"

#include "filesystem.h"
#include "mom_run_poster.h"
#include "mom_shareddefs.h"
#include "mom_api_requests.h"
#include "mom_map_cache.h"

#include <tier0/memdbgon.h>

CRunPoster::CRunPoster()
{
#if ENABLE_STEAM_LEADERBOARDS
    m_hCurrentLeaderboard = 0;
#endif
}

CRunPoster::~CRunPoster() {}

void CRunPoster::PostInit()
{
    // We need to listen for "replay_save"
    ListenForGameEvent("replay_save");
}

void CRunPoster::LevelInitPostEntity()
{
#if ENABLE_STEAM_LEADERBOARDS
    const char *pMapName = MapName();
    if (pMapName)
    {
        CHECK_STEAM_API(SteamUserStats());
        SteamAPICall_t findCall = SteamUserStats()->FindOrCreateLeaderboard(pMapName, k_ELeaderboardSortMethodAscending, k_ELeaderboardDisplayTypeTimeMilliSeconds);
        m_cLeaderboardFindResult.Set(findCall, this, &CRunPoster::OnLeaderboardFind);

    }
#endif
}

void CRunPoster::LevelShutdownPostEntity()
{
#if ENABLE_STEAM_LEADERBOARDS
    m_hCurrentLeaderboard = 0;
#endif
}

void CRunPoster::FireGameEvent(IGameEvent *pEvent)
{
    if (pEvent->GetBool("save"))
    {
#if ENABLE_STEAM_LEADERBOARDS
        CHECK_STEAM_API(SteamUserStats());

        if (!m_hCurrentLeaderboard)
        {
            Warning("Could not upload run: leaderboard doesn't exist!\n");
            // MOM_TODO: Make the run_posted event here with the above message?
            return;
        }

        // Upload the score
        int runTime = pEvent->GetInt("time"); // Time in milliseconds
        if (!runTime)
        {
           Warning("Could not upload run: time is 0 milliseconds!\n");
           // MOM_TODO: Make the run_posted event here with the above message?
           return;
        }

        // Save the name and path for uploading in the callback of the score
        Q_strncpy(m_szFileName, pEvent->GetString("filename"), MAX_PATH);
        Q_strncpy(m_szFilePath, pEvent->GetString("filepath"), MAX_PATH);

        // Set our score
        SteamAPICall_t uploadScore = SteamUserStats()->UploadLeaderboardScore(m_hCurrentLeaderboard, 
            k_ELeaderboardUploadScoreMethodKeepBest, runTime, nullptr, 0);
        m_cLeaderboardScoreUploaded.Set(uploadScore, this, &CRunPoster::OnLeaderboardScoreUploaded);
#endif

        if (CheckCurrentMap())
        {
            CUtlBuffer buf;
            if (g_pFullFileSystem->ReadFile(pEvent->GetString("filepath"), "MOD", buf))
            {
                if (g_pAPIRequests->SubmitRun(g_pMapCache->GetCurrentMapID(), buf, UtlMakeDelegate(this, &CRunPoster::RunSubmitCallback)))
                {
                    DevLog(2, "Run submitted!\n");
                }
                else
                {
                    Warning("Failed to submit run; API call returned false!\n");
                }
            }
            else
            {
                Warning("Failed to submit run: could not read file %s from %s !\n", pEvent->GetString("filename"), pEvent->GetString("filepath"));
            }
        }
    }
}

#if ENABLE_STEAM_LEADERBOARDS
void CRunPoster::OnLeaderboardFind(LeaderboardFindResult_t* pResult, bool bIOFailure)
{
    if (bIOFailure)
    {
        Warning("Failed to create leaderboard for map %s!\n", MapName());
        return;
    }

    m_hCurrentLeaderboard = pResult->m_hSteamLeaderboard;
}

void CRunPoster::OnLeaderboardScoreUploaded(LeaderboardScoreUploaded_t* pResult, bool bIOFailure)
{
    IGameEvent *pEvent = gameeventmanager->CreateEvent("run_upload");

    bool bSuccess = true;
    if (bIOFailure || !pResult->m_bSuccess)
    {
        bSuccess = false;
        // MOM_TODO: If it didn't upload, hijack the run_upload event with a message here?
    }

    if (pEvent)
    {
        pEvent->SetBool("run_posted", bSuccess);

        if (gameeventmanager->FireEvent(pEvent))
        {
            if (bSuccess)
            {
                // Now we can (try to) upload this replay file to the Steam Cloud for attaching to this new leaderboard score
                CUtlBuffer fileBuffer;
                if (filesystem->ReadFile(m_szFilePath, "MOD", fileBuffer))
                {
                    SteamAPICall_t write = SteamRemoteStorage()->FileWriteAsync(m_szFileName, fileBuffer.Base(), fileBuffer.TellPut());
                    m_cFileUploaded.Set(write, this, &CRunPoster::OnFileUploaded);
                }
                else
                {
                    DevWarning("Couldn't read replay file %s!\n", m_szFilePath);
                }

                ConColorMsg(Color(0, 255, 0, 255), "Uploaded run to the leaderboards, check it out!\n");
            }
            else
            {
                Warning("Could not upload your leaderboard score, sorry!\n");
            }
        }
    }
}

void CRunPoster::OnLeaderboardUGCSet(LeaderboardUGCSet_t* pResult, bool bIOFailure)
{
    bool bSuccess = true;
    if (bIOFailure || pResult->m_eResult != k_EResultOK)
    {
        bSuccess = false;
        Warning("Failed to upload replay file to leaderboard! Result: %i\n", pResult->m_eResult);
    }

    // Either way we need to delete the file from Steam Cloud now, don't use quota
    if (SteamRemoteStorage()->FileDelete(m_szFileName))
    {
        DevLog("Successfully deleted the uploaded run on the Steam Cloud at %s\n", m_szFileName);
    }

    // Clear out the paths here
    m_szFileName[0] = 0;
    m_szFilePath[0] = 0;

    if (bSuccess)
        ConColorMsg(Color(0, 255, 0, 255), "Uploaded replay file to leaderboards, check it out!\n");
}

void CRunPoster::OnFileUploaded(RemoteStorageFileWriteAsyncComplete_t* pResult, bool bIOFailure)
{
    if (pResult->m_eResult != k_EResultOK || bIOFailure)
    {
        Warning("Could not upload steam cloud file! Result: %i\n", pResult->m_eResult);
        return;
    }

    SteamAPICall_t UGCcall = SteamRemoteStorage()->FileShare(m_szFileName);
    m_cFileShared.Set(UGCcall, this, &CRunPoster::OnFileShared);
}

void CRunPoster::OnFileShared(RemoteStorageFileShareResult_t* pResult, bool bIOFailure)
{
    if (bIOFailure || pResult->m_eResult != k_EResultOK)
    {
        Warning("Could not upload user replay file! Result %i\n", pResult->m_eResult);
        return;
    }

    // Now we attach to the leaderboard
    SteamAPICall_t UGCLeaderboardCall = SteamUserStats()->AttachLeaderboardUGC(m_hCurrentLeaderboard, pResult->m_hFile);
    m_cLeaderboardUGCSet.Set(UGCLeaderboardCall, this, &CRunPoster::OnLeaderboardUGCSet);
}
#endif

void CRunPoster::RunSubmitCallback(KeyValues* pKv)
{
    IGameEvent *runUploadedEvent = gameeventmanager->CreateEvent("run_upload");
    KeyValues *pData = pKv->FindKey("data");
    KeyValues *pErr = pKv->FindKey("error");
    if (pData)
    {
        // MOM_TODO: parse the data object here

        // Necessary so that the leaderboards and hud_mapfinished update appropriately
        if (runUploadedEvent)
        {
            runUploadedEvent->SetBool("run_posted", true);
            // MOM_TODO: Once the server updates this to contain more info, parse and do more with the response
            gameeventmanager->FireEvent(runUploadedEvent);
        }
    }
    else if (pErr)
    {
        if (runUploadedEvent)
        {
            runUploadedEvent->SetBool("run_posted", false);
            // MOM_TODO: send an error here
            gameeventmanager->FireEvent(runUploadedEvent);
        }
    }
}

bool CRunPoster::CheckCurrentMap()
{
    MapData *pData = g_pMapCache->GetCurrentMapData();
    if (pData && pData->m_uID)
    {
        // Now check if the status is alright
        MAP_UPLOAD_STATUS status = pData->m_eMapStatus;
        if (status == MAP_APPROVED || status == MAP_PRIVATE_TESTING || status == MAP_PUBLIC_TESTING)
        {
            return true;
        }
    }
    return false;
}

static CRunPoster s_momRunposter;
CRunPoster *g_pRunPoster = &s_momRunposter;