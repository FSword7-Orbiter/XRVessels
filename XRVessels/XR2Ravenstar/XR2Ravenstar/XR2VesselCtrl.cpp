// ==============================================================
// Class implementing the XRVesselCtrl interface.
//
// Copyright 2007-2016 Douglas E. Beachy
// All rights reserved.
//
// This software is FREEWARE and may not be sold!
//
// XR2VesselCtrl.cpp
//
// Altea Aerospace: The Future Is Now.
// ==============================================================

#include "XR2Ravenstar.h"

// NOTE: none of these methods perform any significant operations themselves on the internal state of the XR vessel: they call internal methods
// to do any "heavy lifting."  None of the other XRn methods invoke any methods in this file; in other words, these methods are not required
// for operation of the XRn.  They are separate and stand-alone.

// Door State
// returns TRUE if door is valid for this ship
bool XR2Ravenstar::SetDoorState(XRDoorID id, XRDoorState state)            
{
    bool retVal = true;

    switch (id)
    {
    case XRD_PayloadBayDoors:
        ActivateBayDoors(ToDoorStatus(state));
        break;

    // airlock ladder is not supported by the XR2
    case XRD_Ladder:
        return false;   // door not supported

    default:
        // let the superclass handle it
        retVal = DeltaGliderXR1::SetDoorState(id, state);   
        break;
    }

    return retVal;
}

#define SET_IF_NOT_NULL(ptr, value) if (ptr != nullptr) *ptr = value;
// returns XRDS_DoorNotSupported if door does not exist for this ship; if pProc != nullptr, proc is set to 0 <= n <= 1.0
XRDoorState XR2Ravenstar::GetDoorState(XRDoorID id, double *pProc) const 
{
    XRDoorState retVal;

    switch (id)
    {
    case XRD_PayloadBayDoors:
        retVal = ToXRDoorState(bay_status);
        SET_IF_NOT_NULL(pProc, bay_proc);
        break;

    // airlock ladder is not supported by the XR2
    case XRD_Ladder:
        retVal = XRDS_DoorNotSupported;
        SET_IF_NOT_NULL(pProc, -1);
        break;

    default:
        // let the superclass handle it
        retVal = DeltaGliderXR1::GetDoorState(id, pProc);
        break;
    }

    return retVal;
}

// Set the damage status of the XR Vessel; any unsupported fields in 'status' must be set to -1 (for doubles) or XRDMG_NotSupported (for XRDamageState)
bool XR2Ravenstar::SetXRSystemStatus(const XRSystemStatusWrite &status)
{
    // Invoke the superclass to handle all the normal fields
    DeltaGliderXR1::SetXRSystemStatus(status);

    // handle custom fields
    const double bayDoorState = ((status.PayloadBayDoors == XRDMG_online) ? 1.0 : 0.0);
    SetDamageStatus(BayDoors, bayDoorState);

    // check the user attempting to set any unsupported fields
    bool retVal = true;
    retVal &= !(status.CrewElevator != XRDMG_NotSupported);

    return retVal;
}

// Read the status of the XR vessel
void XR2Ravenstar::GetXRSystemStatus(XRSystemStatusRead &status)
{
    // Invoke the superclass to fill in the base values; this must be invoked *before* we populate our custom values.
    DeltaGliderXR1::GetXRSystemStatus(status);

    status.PayloadBayDoors = ((GetDamageStatus(BayDoors).fracIntegrity == 1.0) ? XRDMG_online : XRDMG_offline);
}

