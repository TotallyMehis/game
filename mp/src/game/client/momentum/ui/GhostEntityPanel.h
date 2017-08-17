#pragma once

#include "cbase.h"
#include "c_mom_online_ghost.h"
#include "vgui_entitypanel.h"
#include "vgui_avatarimage.h"
#include "hudelement.h"

class C_MomentumOnlineGhostEntity;

class CGhostEntityPanel : public vgui::Panel
{
    DECLARE_CLASS_SIMPLE(CGhostEntityPanel, vgui::Panel);

    CGhostEntityPanel();
    ~CGhostEntityPanel();

    void Init(C_MomentumOnlineGhostEntity *pEntity);
    void OnThink() OVERRIDE;
    void OnTick() OVERRIDE;

    bool ShouldDraw();

    void SetShouldDrawEntityName(bool bState) { m_bPaintName = bState; }

    void GetEntityPosition(int& sx, int& sy);
    void ComputeAndSetSize();

private:

    bool m_bPaintName;

    C_MomentumOnlineGhostEntity *m_pEntity;
    vgui::Label *m_pNameLabel;
    vgui::ImagePanel *m_pAvatarImagePanel;
    CAvatarImage *m_pAvatarImage;

    int				m_iOrgWidth;
    int				m_iOrgHeight;
    int				m_iOrgOffsetX;
    int				m_iOrgOffsetY;
    float			m_flScale;
    // Offset from entity that we should draw
    int m_OffsetX, m_OffsetY;
};