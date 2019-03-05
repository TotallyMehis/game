#pragma once

#include "weapon_csbase.h"

// This is the base class for pistols and rifles.
#if defined(CLIENT_DLL)
#define CWeaponCSBaseGun C_WeaponCSBaseGun
#endif

class CWeaponCSBaseGun : public CWeaponCSBase
{
  public:
    DECLARE_CLASS(CWeaponCSBaseGun, CWeaponCSBase);
    DECLARE_NETWORKCLASS();
    DECLARE_PREDICTABLE();

    CWeaponCSBaseGun();

    virtual void PrimaryAttack();
    virtual void Spawn();
    virtual bool Deploy();
#ifdef WEAPONS_USE_AMMO
    virtual bool Reload();
#endif
    virtual void WeaponIdle();

    // Derived classes call this to fire a bullet.
    bool CSBaseGunFire(float flSpread, float flCycleTime, bool bPrimaryMode);

    // Usually plays the shot sound. Guns with silencers can play different sounds.
    virtual void DoFireEffects();
    virtual void ItemPostFrame();
    // Mostly takes care of lowering the weapon
    virtual void ProcessAnimationEvents();

  protected:
    float m_zoomFullyActiveTime;
    float m_flTimeToIdleAfterFire;
    float m_flIdleInterval;
    // Is the weapon currently lowered?
    bool m_bWeaponIsLowered;

  private:
    CWeaponCSBaseGun(const CWeaponCSBaseGun &);
};