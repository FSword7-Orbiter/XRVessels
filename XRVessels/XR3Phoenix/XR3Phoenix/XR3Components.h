/**
  XR Vessel add-ons for OpenOrbiter Space Flight Simulator
  Copyright (C) 2006-2021 Douglas Beachy

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.

  Email: mailto:doug.beachy@outlook.com
  Web: https://www.alteaaerospace.com
**/

// ==============================================================
// XR3Phoenix implementation class
//
// XR3Components.h
// XR3Phoenix components.
// ==============================================================

#pragma once

#include "orbitersdk.h"
#include "XR1Component.h"
#include "XR3Areas.h"
#include "XR1Areas.h"
#include "XR1MultiDisplayArea.h"

// 
// Components
//

class XR3WarningLightsComponent : public XR1Component
{
public:
    XR3WarningLightsComponent(InstrumentPanel &instrumentPanel, COORD2 topLeft);
};

class XR3ActiveEVAPortComponent : public XR1Component
{
public:
    XR3ActiveEVAPortComponent(InstrumentPanel &instrumentPanel, COORD2 topLeft);
};

//
// Areas begin here
//

//----------------------------------------------------------------------------------

class XR3MWSTestButtonArea : public MomentaryButtonArea
{
public:
    XR3MWSTestButtonArea(InstrumentPanel &parentPanel, const COORD2 panelCoordinates, const int areaID);

protected:
    virtual void ProcessButtonAction(int event, double buttonDownSimt);
};

//----------------------------------------------------------------------------------

class XR3WarningLightsArea : public XR1Area
{
public:
    XR3WarningLightsArea(InstrumentPanel &parentPanel, const COORD2 panelCoordinates, const int areaID);
    virtual void Activate();
    virtual bool Redraw2D(const int event, const SURFHANDLE surf);

protected:
    virtual void clbkPrePostStep(const double simt, const double simdt, const double mjd);

    // state data
    bool m_lightStateOn;  // true if light state (during blink) is ON
};


//----------------------------------------------------------------------------------
// our custom hull temps multi-display mode
class XR3HullTempsMultiDisplayMode : public HullTempsMultiDisplayMode
{
public:
    XR3HullTempsMultiDisplayMode(int modeNumber);

protected:
    virtual double GetHighestTempFrac();

    // if DOOR_OPEN, temperature values will be displayed in yellow or red correctly since that door is open
    virtual DoorStatus GetNoseDoorStatus();
    virtual DoorStatus GetLeftWingDoorStatus();
    virtual DoorStatus GetRightWingDoorStatus();
    virtual DoorStatus GetTopHullDoorStatus();
};

//----------------------------------------------------------------------------------

class DockingPortActiveLEDArea : public XR1Area
{
public:
    DockingPortActiveLEDArea(InstrumentPanel &parentPanel, const COORD2 panelCoordinates, const int areaID);

protected:
    virtual void Activate();
    virtual bool Redraw2D(const int event, const SURFHANDLE surf);
};

//----------------------------------------------------------------------------------

class CrewElevatorActiveLEDArea : public XR1Area
{
public:
    CrewElevatorActiveLEDArea(InstrumentPanel &parentPanel, const COORD2 panelCoordinates, const int areaID);

protected:
    virtual void Activate();
    virtual bool Redraw2D(const int event, const SURFHANDLE surf);
};

//----------------------------------------------------------------------------------

class ActiveEVAPortSwitchArea : public HorizontalCenteringRockerSwitchArea
{
public:
    ActiveEVAPortSwitchArea(InstrumentPanel &parentPanel, const COORD2 panelCoordinates, const int areaID);

protected:
    virtual void ProcessSwitchEvent(SWITCHES switches, POSITION position);
    virtual bool Redraw2D(const int event, const SURFHANDLE surf);
};

