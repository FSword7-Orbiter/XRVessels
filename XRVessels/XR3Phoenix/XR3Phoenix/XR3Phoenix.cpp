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
// XR3Phoenix.cpp
// ==============================================================

#define ORBITER_MODULE

#include "XR3Phoenix.h"
#include "DlgCtrl.h"
#include <stdio.h>

#include "XR3AreaIDs.h"  
#include "XR3InstrumentPanels.h"
#include "XR1PreSteps.h"
#include "XR1PostSteps.h"
#include "XR1FuelPostSteps.h"
#include "XR1AnimationPoststep.h"

#include "XR3Globals.h"
#include "XR3PreSteps.h"
#include "XR3PostSteps.h"
#include "XRPayload.h"
#include "XR3PayloadBay.h"

#include "meshres.h"

// ==============================================================
// API callback interface
// ==============================================================

// --------------------------------------------------------------
// Module initialisation
// --------------------------------------------------------------
DLLCLBK void InitModule (HINSTANCE hModule)
{
    g_hDLL = hModule;
    oapiRegisterCustomControls(hModule);
}

// --------------------------------------------------------------
// Module cleanup
// --------------------------------------------------------------
DLLCLBK void ExitModule (HINSTANCE hModule)
{
    oapiUnregisterCustomControls(hModule);
    XRPayloadClassData::Terminate();     // clean up global cache
}

// --------------------------------------------------------------
// Vessel initialisation
// --------------------------------------------------------------
DLLCLBK VESSEL *ovcInit (OBJHANDLE vessel, int flightmodel)
{
#ifdef _DEBUG
    // NOTE: _CRTDBG_CHECK_ALWAYS_DF is too slow
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF |
                   _CRTDBG_CHECK_CRT_DF | 
                   _CRTDBG_LEAK_CHECK_DF); 
#endif

    return new XR3Phoenix(vessel, flightmodel, new XR3ConfigFileParser());
}

// --------------------------------------------------------------
// Vessel cleanup
// --------------------------------------------------------------

// NOTE: must receive this as a VESSEL2 ptr because that's how Orbiter calls it
DLLCLBK void ovcExit(VESSEL2 *vessel)
{
    // NOTE: in order to free up VESSEL3 data, you must add an empty virtual destructor to the VESSEL2 class in VesselAPI.h
    
    // This is a hack so that the VESSEL3_EXT, XR3 and VESSEL3 destructors will be invoked.
    // Invokes XR3 destructor -> XR1 destructor -> VESSEL3_EXT destructor -> VESSEL3 destructor
    VESSEL3_EXT *pXR3 = reinterpret_cast<VESSEL3_EXT *>( (((void **)vessel)-1) );  // bump vptr to VESSEL3_EXT subclass, which has a virtual destructor
    delete pXR3;
}

// ==============================================================
// Airfoil coefficient functions
// Return lift, moment and zero-lift drag coefficients as a
// function of angle of attack (alpha or beta)
// ==============================================================

///#define PROFILE_DRAG 0.015
// NEW to fix "floaty" landings
// XR3 ORG: #define PROFILE_DRAG 0.030
// improve glide performance for the Vanguard
#define PROFILE_DRAG 0.015

// 1. vertical lift component (wings and body)
void VLiftCoeff (VESSEL *v, double aoa, double M, double Re, void *context, double *cl, double *cm, double *cd)
{
    const int nabsc = 9;
    static const double AOA[nabsc] =         {-180*RAD,-60*RAD,-30*RAD, -1*RAD, 15*RAD,20*RAD,25*RAD,50*RAD,180*RAD};

    static const double CL[nabsc]  =         {       0,      0,   -0.15,      0,    0.7,     0.5, 0.2,     0,      0};  // decrease negative lift to fix nose-down attitude hold problems

    static const double CM[nabsc]  =         {       0,      0,  0.014, 0.0039, -0.006,-0.008,-0.010,     0,      0};

    int i=0;    
    for (i = 0; i < nabsc-1 && AOA[i+1] < aoa; i++);
    double f = (aoa-AOA[i]) / (AOA[i+1]-AOA[i]);
    *cl = CL[i] + (CL[i+1]-CL[i]) * f;  // aoa-dependent lift coefficient
    *cm = CM[i] + (CM[i+1]-CM[i]) * f;  // aoa-dependent moment coefficient
    double saoa = sin(aoa);
    double pd = PROFILE_DRAG + 0.4*saoa*saoa;  // profile drag
    *cd = pd + oapiGetInducedDrag (*cl, WING_ASPECT_RATIO, WING_EFFICIENCY_FACTOR) + oapiGetWaveDrag (M, 0.75, 1.0, 1.1, 0.04);
    // profile drag + (lift-)induced drag + transonic/supersonic wave (compressibility) drag
}

// 2. horizontal lift component (vertical stabilisers and body)

void HLiftCoeff (VESSEL *v, double beta, double M, double Re, void *context, double *cl, double *cm, double *cd)
{
    const int nabsc = 8;
    static const double BETA[nabsc] = {-180*RAD,-135*RAD,-90*RAD,-45*RAD,45*RAD,90*RAD,135*RAD,180*RAD};
    static const double CL[nabsc]   = {       0,    +0.3,      0,   -0.3,  +0.3,     0,   -0.3,      0};

    int i=0;    
    for (i = 0; i < nabsc-1 && BETA[i+1] < beta; i++);
    *cl = CL[i] + (CL[i+1]-CL[i]) * (beta-BETA[i]) / (BETA[i+1]-BETA[i]);
    *cm = 0.0;
    *cd = PROFILE_DRAG + oapiGetInducedDrag (*cl, 1.5, 0.6) + oapiGetWaveDrag (M, 0.75, 1.0, 1.1, 0.04);
}

//
// Constructor
//
XR3Phoenix::XR3Phoenix(OBJHANDLE hObj, int fmodel, XR3ConfigFileParser *pConfigFileParser) : 
    DeltaGliderXR1(hObj, fmodel, pConfigFileParser),
    m_rcsDockingMode(false), m_rcsDockingModeAtKillrotStart(false),
    m_hiddenElevatorTrimState(0), m_activeEVAPort(ACTIVE_EVA_PORT::DOCKING_PORT)
{
    // init new XR3 warning lights
    for (int i=0; i < XR3_WARNING_LIGHT_COUNT; i++)
        m_XR3WarningLights[i] = false;  // not lit

    // init new doors
    crewElevator_status = DoorStatus::DOOR_CLOSED;
    crewElevator_proc   = 0.0;
    bay_status          = DoorStatus::DOOR_CLOSED;
    bay_proc            = 0.0;

    // XR3TODO: define VC font
    // replace the data HUD font with a smaller one
    // XR1 ORG: m_pDataHudFont = CreateFont(20, 0, 0, 0, 700, 0, 0, 0, 0, 0, 0, NONANTIALIASED_QUALITY, 0, "Tahoma");
    // XR1 ORG: m_pDataHudFontSize = 22;      // includes spacing
    /* MATCHES XR1 FONT NOW 
    oapiReleaseFont(m_pDataHudFont);        // free existing front created by the XR1's constructor
    m_pDataHudFont = oapiCreateFont(22, true, "Tahoma", FONT_BOLD);  // will be freed by the XR1's destructor
    m_pDataHudFontSize = 18;      // includes spacing
    */
}

// 
// Destructor
//
XR3Phoenix::~XR3Phoenix()
{
    // Note: our superclass handles cleanup of m_pPayloadBay and s_hPayloadEditorDialog
}

// ==============================================================
// Overloaded callback functions
// ==============================================================

// --------------------------------------------------------------
// Set vessel class parameters
// --------------------------------------------------------------
void XR3Phoenix::clbkSetClassCaps (FILEHANDLE cfg)
{
    // parse the configuration file
    // If parse fails, we shouldn't display a MessageBox here because the Orbiter main window 
    // keeps putting itself in the foreground, covering it up and making Orbiter look like it's hung.
    // Therefore, TakeoffAndLandingCalloutsAndCrashPostStep will blink a warning message for us 
    // if the parse fails.
    ParseXRConfigFile();   // common XR code

    // Note: this must be invoked here instead of the constructor so the we may override it!
    DefineAnimations();

    // define our payload bay and attachment points
    CreatePayloadBay();

    // *************** physical parameters **********************
    
    ramjet = new XR1Ramjet(this); 
    
    VESSEL2::SetEmptyMass(EMPTY_MASS);
    SetSize (14.745);    // 1/2 ship's total width
    SetVisibilityLimit (7.5e-4, 1.5e-3);
    SetAlbedoRGB (_V(0.13,0.20,0.77));   // bluish
    SetGravityGradientDamping (20.0);    // ? same as DG for now
    
    SetCrossSections(_V(147.97, 486.33, 63.01));

    SetMaxWheelbrakeForce(MAX_WHEELBRAKE_FORCE);  

    SetPMI(_V(88.20, 107.35, 27.03));
    
    SetDockParams (DOCKING_PORT_COORD, _V(0,1,0), _V(0,0,-1));   // top-mounted port

    SetGearParameters(1.0);  // NOTE: must init touchdown points here with gear DOWN!  This will be called again later by clbkPostCreation to init the "real" state from scenario file.

    EnableTransponder (true);
    SetTransponderChannel(207); // XPDR = 118.35 MHz

    // init APU runtime callout timestamp
    MarkAPUActive();  // reset the APU idle warning callout time

    // enable IDS so we transmit a docking signal
    DOCKHANDLE hDock = GetDockHandle(0);    // primary docking port
    EnableIDS(hDock, true);
    SetIDSChannel(hDock, 209);  // DOCK = 113.45 MHz
    
    // ******************** Attachment points **************************

    // top-center (for lifter attachment)
    // SET IN CONFIG FILE: CreateAttachment(true, _V(0,0,0), _V(0,-1,0), _V(0,0,1), "XS");

    // ******************** NAV radios **************************
    
    InitNavRadios(4);
    
    // ****************** propellant specs **********************
    
    // set tank configuration
    max_rocketfuel = TANK1_CAPACITY;
    max_scramfuel  = TANK2_CAPACITY;

    // NOTE: Orbiter seems to reset this to zero later, since he expects the scenario file to be read.
    // WARNING: do NOT init these values to > 0, because Orbiter will NOT set the tank value if the fraction is 
    // zero in the scenario file.
    ph_main  = CreatePropellantResource(max_rocketfuel);      // main tank (fuel + oxydant)
    ph_rcs   = CreatePropellantResource(RCS_FUEL_CAPACITY);   // RCS tank  (fuel + oxydant)
    ph_scram = CreatePropellantResource(max_scramfuel);       // scramjet fuel

    // **************** thruster definitions ********************
    
    double ispscale = (GetXR1Config()->EnableATMThrustReduction ? 0.8 : 1.0);
    // Reduction of thrust efficiency at normal pressure
    
    // increase level, srcrate, and lifetime
    // XR5: const double particleMult = 1.5;
    const double particleMult = 1.0;    // XR3TODO: tweak this
    PARTICLESTREAMSPEC contrail = {
        0, 11.0 * particleMult, 6 * particleMult, 150, 0.3, 7.5, 4, 3.0, PARTICLESTREAMSPEC::DIFFUSE,
            PARTICLESTREAMSPEC::LVL_PSQRT, 0, 2,
            PARTICLESTREAMSPEC::ATM_PLOG, 1e-4, 1
    };
    
    // increase level
    PARTICLESTREAMSPEC exhaust_main = {
        0, 3.0 * particleMult, 10 * particleMult, 150, 0.1, 0.2, 16, 1.0, PARTICLESTREAMSPEC::EMISSIVE,
            PARTICLESTREAMSPEC::LVL_SQRT, 0, 1,
            PARTICLESTREAMSPEC::ATM_PLOG, 1e-5, 0.1
    };
    // increase level
    PARTICLESTREAMSPEC exhaust_hover = {
        0, 2.0 * particleMult, 10 * particleMult, 150, 0.1, 0.15, 16, 1.0, PARTICLESTREAMSPEC::EMISSIVE,
            PARTICLESTREAMSPEC::LVL_SQRT, 0, 1,
            PARTICLESTREAMSPEC::ATM_PLOG, 1e-5, 0.1
    };
    // increase level and particle lifetime
     PARTICLESTREAMSPEC exhaust_scram = {
        0, 3.0 * particleMult, 25 * particleMult, 150, 0.05, 15.0, 3, 1.0, PARTICLESTREAMSPEC::EMISSIVE,
            PARTICLESTREAMSPEC::LVL_SQRT, 0, 1,
            PARTICLESTREAMSPEC::ATM_PLOG, 1e-5, 0.1
    };

#if 0  // not used
    // RCS
    PARTICLESTREAMSPEC exhaust_rcs = {
        0,          // flags
        0.01,       // particle size
        15,         // particle create rate (Hz)
        25,         // emission velocity
        0.03,       // velocity spread during creation
        0.55,       // average particle lifetime
        1.5,        // particle growth rate
        0.10,       // slowdown in ATM (m/s)
        PARTICLESTREAMSPEC::EMISSIVE, PARTICLESTREAMSPEC::LVL_SQRT, 0, 1, PARTICLESTREAMSPEC::ATM_PLOG, 1e-5, 0.1
    };
    // retros
    PARTICLESTREAMSPEC exhaust_retro = {
        0,          // flags
        0.19,       // particle size
        65,         // particle creation rate (Hz)
        60,         // emission velocity
        0.13,       // velocity spread during creation
        1.50,       // average particle lifetime
        2.0,        // particle growth rate
        0.40,       // slowdown in ATM (m/s)
        PARTICLESTREAMSPEC::EMISSIVE, PARTICLESTREAMSPEC::LVL_SQRT, 0, 1, PARTICLESTREAMSPEC::ATM_PLOG, 1e-5, 0.1
    };
#endif


    // handle new configurable ISP
    const double mainISP = GetXR1Config()->GetMainISP();

    /* From API Guide:
        Vessel coordinates are always defined so that the CG is at the origin (0,0,0). Therefore, a
        thruster located at (0,0,-10) and generating thrust in direction (0,0,1) would not generate
        torque.
    */

    // define thruster locations in meters from the ship's centerpoint
    const double shipLength = 36.75;
    const double rcsZHullDistance = (shipLength / 2) - 4.0;   // distance from Z centerline -> RCS fore and aft
    // XR3TODO: tweak this via rotation testing
    const double rcsXWingDistance = 12.0;                     // distance from X centerline -> simulated RCS on wings (not modeled visually) [XR2 was 8.0] : we cheat a bit here to improve rotation performance

    // main thrusters
    const double mainEngineZ = -(shipLength / 2) - 1;
    th_main[0] = CreateThruster(_V(-3.59, 0, mainEngineZ), _V(0, 0, 1), MAX_MAIN_THRUST[GetXR1Config()->MainEngineThrust], ph_main, mainISP, mainISP*ispscale);
    th_main[1] = CreateThruster(_V(3.59, 0, mainEngineZ), _V(0, 0, 1), MAX_MAIN_THRUST[GetXR1Config()->MainEngineThrust], ph_main, mainISP, mainISP*ispscale);

    thg_main = CreateThrusterGroup (th_main, 2, THGROUP_MAIN);
    SURFHANDLE mainExhaustTex = oapiRegisterExhaustTexture("XR3Phoenix\\ExhaustXR3");
    const double mainLscale = 12;  
    const double mainWscale = 1.2;   // RADIUS
    const double mainExhaustZCoord = -13.5;  // to show the exhaust texture better

#define ADD_MAIN_EXHAUST(th, x, y)  \
    AddXRExhaust (th, mainLscale, mainWscale, _V(x, y, mainExhaustZCoord), _V(0,0,-1), mainExhaustTex); \
    AddExhaustStream (th, _V(x, y, mainExhaustZCoord - 13), &exhaust_main);                           \
    AddExhaustStream (th, _V(x, y, mainExhaustZCoord - 20), &contrail)

    // left side (viewed from rear)
    ADD_MAIN_EXHAUST(th_main[0], -7.25, 0);  // outboard
    ADD_MAIN_EXHAUST(th_main[0], -5.75, 0);  // inboard
    
    // right side (viewed from rear)
    ADD_MAIN_EXHAUST(th_main[1], 7.25, 0);  // outboard
    ADD_MAIN_EXHAUST(th_main[1], 5.75, 0);  // inboard

    // retro thrusters
    const double retroXCoord = 3.946;
    const double retroYCoord = 0.25;
    const double retroZCoord = 13.347;
    // Note: we use zero for the engine Y coordinate here to balance the thrust; this has nothing to do with the four visible retro engine exhausts
    th_retro[0] = CreateThruster (_V(-retroXCoord, 0, retroZCoord), _V(0, 0, -1.0), MAX_RETRO_THRUST, ph_main, mainISP, mainISP*ispscale);
    th_retro[1] = CreateThruster (_V( retroXCoord, 0, retroZCoord), _V(0, 0, -1.0), MAX_RETRO_THRUST, ph_main, mainISP, mainISP*ispscale);

    const double retroLscale = 3.0;
    const double retroWscale = 0.5;

#define ADD_RETRO_EXHAUST(th, x, y) \
    AddXRExhaust (th, retroLscale, retroWscale, _V(x, retroYCoord, retroZCoord), _V(0,0,1), mainExhaustTex);  \
    /* not used: AddExhaustStream (th, _V(x, retroYCoord, retroZCoord + 0.5), &exhaust_retro) */

    thg_retro = CreateThrusterGroup (th_retro, 2, THGROUP_RETRO);
    // add the four retro exhaust flames
    ADD_RETRO_EXHAUST(th_retro[0], -retroXCoord, retroYCoord);    
    ADD_RETRO_EXHAUST(th_retro[0], -retroXCoord, -retroYCoord);
    ADD_RETRO_EXHAUST(th_retro[1], retroXCoord, retroYCoord);
    ADD_RETRO_EXHAUST(th_retro[1], retroXCoord, -retroYCoord);

    // hover thrusters (simplified)
    // The two aft hover engines are combined into a single "logical" thruster,
    // but exhaust is rendered separately for both
    const double hoverZ = 10.6;
    th_hover[0] = CreateThruster(_V(0, 0,  hoverZ), _V(0, 1, 0), MAX_HOVER_THRUST[GetXR1Config()->HoverEngineThrust], ph_main, mainISP, mainISP*ispscale);
    th_hover[1] = CreateThruster(_V(0, 0, -hoverZ), _V(0, 1, 0), MAX_HOVER_THRUST[GetXR1Config()->HoverEngineThrust], ph_main, mainISP, mainISP*ispscale);
    thg_hover = CreateThrusterGroup (th_hover, 2, THGROUP_HOVER);

    const double hoverLscale = 2.0;  // shorter (old were too long)
    const double hoverWscale = 0.8; 

#define ADD_HOVER_EXHAUST(th, x, y, z)  \
    AddXRExhaust (th, hoverLscale, hoverWscale, _V(x, y, z), _V(0,-1,0), mainExhaustTex); \
    AddExhaustStream (th, _V(x, y - 4.5, z), &exhaust_hover);                             \
    AddExhaustStream (th, _V(x, y - 7, z), &contrail)

    // define eight hover engine flames & particle streams
    // forward
    ADD_HOVER_EXHAUST(th_hover[0],  1.6, -1.1, 10.6);
    ADD_HOVER_EXHAUST(th_hover[0], -1.6, -1.1, 10.6);
    ADD_HOVER_EXHAUST(th_hover[0],  1.6, -1.1, 9.4);
    ADD_HOVER_EXHAUST(th_hover[0], -1.6, -1.1, 9.4);

    // aft 
    ADD_HOVER_EXHAUST(th_hover[1],  6.5, -0.9, -8.35);
    ADD_HOVER_EXHAUST(th_hover[1], -6.5, -0.9, -8.35);
    ADD_HOVER_EXHAUST(th_hover[1],  6.5, -0.9, -9.5);
    ADD_HOVER_EXHAUST(th_hover[1], -6.5, -0.9, -9.5);

    // set of attitude thrusters (idealised). The arrangement is such that no angular
    // momentum is created in linear mode, and no linear momentum is created in rotational mode.
    SURFHANDLE rcsExhaustTex = mainExhaustTex;

    // create RCS thrusters (not related to RCS exhaust)
    th_rcs[0] = CreateThruster (_V(0, 0, rcsZHullDistance), _V(0, 1, 0), GetRCSThrustMax(0), ph_rcs, mainISP);  // fore bottom (i.e., pushes UP from the BOTTOM of the hull)
    th_rcs[1] = CreateThruster (_V(0, 0,-rcsZHullDistance), _V(0,-1, 0), GetRCSThrustMax(1), ph_rcs, mainISP);  // aft top
    th_rcs[2] = CreateThruster (_V(0, 0, rcsZHullDistance), _V(0,-1, 0), GetRCSThrustMax(2), ph_rcs, mainISP);  // fore top
    th_rcs[3] = CreateThruster (_V(0, 0,-rcsZHullDistance), _V(0, 1, 0), GetRCSThrustMax(3), ph_rcs, mainISP);  // aft bottom

    const double rcsLscale =  1.0;      // XR2 was 0.6
    const double rcsWscale = 0.11;      // XR2 was 0.07

    // these are for the larger RCS jets
    const double rcsLscaleLarge = 1.5;      
    const double rcsWscaleLarge = 0.16;     

    const double rcsDepthModifier = 0;      // reduce depth of thruster flame firing so it shows up better
    const double rcsNoseDepthModifier = 0;  // top-mounted Y-axis nose RCS jets are deeper than standard jets
    const double rcsTailDepthModifier = 0;  // rear-mounted Z-axis RCS jets are deeper than standard jets

    const double exhaustDistance = 1.4;        // exhaust distance from thruster coordinates; XR2 was 1.4

// move exhaust smoke away from the RCS jet by a fixed distance
#define ADD_RCS_EXHAUST(th, coordsV, directionV)                                     \
        AddXRExhaust (th, rcsLscale, rcsWscale, coordsV, directionV, rcsExhaustTex)

#define ADD_LARGE_RCS_EXHAUST(th, coordsV, directionV)                                \
    AddXRExhaust (th, rcsLscaleLarge, rcsWscaleLarge, coordsV, directionV, rcsExhaustTex)
        
// compute actual RCS depth coordinate; this is necessary for hull-mounted RCS jets
#define RCS_DCOORD(c, dir) (c + (dir * rcsDepthModifier))
#define NOSE_RCS_DCOORD(c, dir) (c + (dir * rcsNoseDepthModifier))
#define TAIL_RCS_DCOORD(c, dir) (c + (dir * rcsTailDepthModifier))
    
    // fore bottom 
    // Note: the direction for these thrusters is a little wonky (not (0,-1,0) as normal), I think because Loru combined rotate ("bank") and pitch/translation in one thruster.
    ADD_LARGE_RCS_EXHAUST(th_rcs[0], _V(2.097, RCS_DCOORD(0.333, -1), 19.032), _V(0.643, -0.766, 0));     // ---->>> Front set: Pitch up / Bank Right / translation up
    ADD_LARGE_RCS_EXHAUST(th_rcs[0], _V(2.221, RCS_DCOORD(0.333, -1), 18.556), _V(0.643, -0.766, 0));

    ADD_LARGE_RCS_EXHAUST(th_rcs[0], _V(-2.097, RCS_DCOORD(0.333, -1), 19.032), _V(-0.643, -0.766, 0));   // ---->>> Front set: Pitch up / Bank left / translation up
    ADD_LARGE_RCS_EXHAUST(th_rcs[0], _V(-2.221, RCS_DCOORD(0.333, -1), 18.556), _V(-0.643, -0.766, 0));

    // aft top
    const double aftPitchXDelta = 8.25;  // Loru's supplied RCS coordinates of 8.5 were off for these jets, so I had to adjust them manually; hence the variable. 
    ADD_RCS_EXHAUST(th_rcs[1], _V(-aftPitchXDelta, RCS_DCOORD(0.45, 1), -10.693), _V(0, 1, 0));    // ---->>> Rear Top set: Pitch up / translation down / Bank Left
    ADD_RCS_EXHAUST(th_rcs[1], _V(-aftPitchXDelta, RCS_DCOORD(0.45, 1), -11.077), _V(0, 1, 0));

    ADD_RCS_EXHAUST(th_rcs[1], _V(aftPitchXDelta, RCS_DCOORD(0.45, 1), -10.693), _V(0, 1, 0));     // ---->>> Rear Top set: Pitch UP / translation down / Bank Right
    ADD_RCS_EXHAUST(th_rcs[1], _V(aftPitchXDelta, RCS_DCOORD(0.45, 1), -11.077), _V(0, 1, 0));

    // fore top
    ADD_RCS_EXHAUST(th_rcs[2], _V(-0.23, NOSE_RCS_DCOORD(0.95, 1), 20.248), _V(0, 1, 0));      // ---->>> Front set: Pitch down / translation down
    ADD_RCS_EXHAUST(th_rcs[2], _V( 0.23, NOSE_RCS_DCOORD(0.95, 1), 20.248), _V(0, 1, 0));
    // XR3TODO: we may be missing a pair of RCS definitions here; need to test visually

    // aft bottom
    ADD_RCS_EXHAUST(th_rcs[3], _V(aftPitchXDelta, RCS_DCOORD(-0.4, -1), -10.693), _V(0, -1, 0));     // ---->>> Rear Bottom set: Pitch down /translation up / Bank left
    ADD_RCS_EXHAUST(th_rcs[3], _V(aftPitchXDelta, RCS_DCOORD(-0.4, -1), -11.077), _V(0, -1, 0));

    ADD_RCS_EXHAUST(th_rcs[3], _V(-aftPitchXDelta, RCS_DCOORD(-0.4, -1), -10.693), _V(0, -1, 0));     // ---->>> Rear Bottom set: Pitch down /translation up / Bank Right
    ADD_RCS_EXHAUST(th_rcs[3], _V(-aftPitchXDelta, RCS_DCOORD(-0.4, -1), -11.077), _V(0, -1, 0));

    th_rcs[4] = CreateThruster (_V(0, 0, rcsZHullDistance), _V(-1,0,0), GetRCSThrustMax(4), ph_rcs, mainISP);  // fore right side
    th_rcs[5] = CreateThruster (_V(0, 0,-rcsZHullDistance), _V( 1,0,0), GetRCSThrustMax(5), ph_rcs, mainISP);  // aft left side
    th_rcs[6] = CreateThruster (_V(0, 0, rcsZHullDistance), _V( 1,0,0), GetRCSThrustMax(6), ph_rcs, mainISP);  // fore left side
    th_rcs[7] = CreateThruster (_V(0, 0,-rcsZHullDistance), _V(-1,0,0), GetRCSThrustMax(7), ph_rcs, mainISP);  // aft right side

    // fore right side
    ADD_RCS_EXHAUST(th_rcs[4], _V(RCS_DCOORD(2.55, 1),  0.167, 17.949), _V(1, 0, 0));   // ---->>> Front set: Yaw Left / Translation Left
    ADD_RCS_EXHAUST(th_rcs[4], _V(RCS_DCOORD(2.55, 1), -0.224, 17.949), _V(1, 0, 0));

    // aft left side
    ADD_RCS_EXHAUST(th_rcs[5], _V(RCS_DCOORD(-7.9, -1.0), 0.7, -10.9), _V(-1, 0, 0));  // ---->>> Rear side set: Yaw left / translation right
    ADD_RCS_EXHAUST(th_rcs[5], _V(RCS_DCOORD(-7.9, -1.0), 0.7, -10.6), _V(-1, 0, 0));
    ADD_RCS_EXHAUST(th_rcs[5], _V(RCS_DCOORD(-7.9, -1.0), 0.7, -10.3), _V(-1, 0, 0));

    // fore left side
    ADD_RCS_EXHAUST(th_rcs[6], _V(RCS_DCOORD(-2.55, -1),  0.167, 17.949), _V(-1, 0, 0));  // ---->>> Front set: Yaw Right / Translation Right
    ADD_RCS_EXHAUST(th_rcs[6], _V(RCS_DCOORD(-2.55, -1), -0.224, 17.949), _V(-1, 0, 0));
    
    // aft right side
    ADD_RCS_EXHAUST(th_rcs[7], _V(RCS_DCOORD(7.9, 1.0), 0.7, -10.9), _V(1, 0, 0));  // ---->>> Rear side set: Yaw right / translation left
    ADD_RCS_EXHAUST(th_rcs[7], _V(RCS_DCOORD(7.9, 1.0), 0.7, -10.6), _V(1, 0, 0));
    ADD_RCS_EXHAUST(th_rcs[7], _V(RCS_DCOORD(7.9, 1.0), 0.7, -10.3), _V(1, 0, 0));

    // Define rotation thrusters (we cheat a bit here and put the rotation thrusters out on the wings, even though they aren't there on the mesh.  :)
    th_rcs[8]  = CreateThruster (_V( rcsXWingDistance, 0, 0), _V(0, 1,0), GetRCSThrustMax(8),  ph_rcs, mainISP);    // right wing bottom
    th_rcs[9]  = CreateThruster (_V(-rcsXWingDistance, 0, 0), _V(0,-1,0), GetRCSThrustMax(9),  ph_rcs, mainISP);    // left wing top
    th_rcs[10] = CreateThruster (_V(-rcsXWingDistance, 0, 0), _V(0, 1,0), GetRCSThrustMax(10), ph_rcs, mainISP);    // left wing bottom
    th_rcs[11] = CreateThruster (_V( rcsXWingDistance, 0, 0), _V(0,-1,0), GetRCSThrustMax(11), ph_rcs, mainISP);    // right wing top

    // Rotation exhaust: note that these exhausts share coordinates with other thrusters, since they do "double-duty."
    // These are logically mounted on the wings, but we re-use hull jets on the side to rotate the ship along the Z axis
    ADD_RCS_EXHAUST(th_rcs[8], _V(8.5, -0.4, -10.693), _V(0, -1, 0));  // right side bottom : ---->>> Rear Bottom set: Pitch down / translation up / Bank left
    ADD_RCS_EXHAUST(th_rcs[8], _V(8.5, -0.4, -11.077), _V(0, -1, 0));

    ADD_RCS_EXHAUST(th_rcs[9], _V(-8.5, 0.45, -10.693), _V(0, 1, 0));   // left side top : ----->>> Rear Top set: Pitch up / translation down / Bank Left
    ADD_RCS_EXHAUST(th_rcs[9], _V(-8.5, 0.45, -11.077), _V(0, 1, 0));

    ADD_RCS_EXHAUST(th_rcs[10], _V(-8.5, -0.4, -10.693), _V(0, -1, 0));  // left side bottom : ---->>> Rear Bottom set: Pitch down / translation up / Bank Right
    ADD_RCS_EXHAUST(th_rcs[10], _V(-8.5, -0.4, -11.077), _V(0, -1, 0));

    ADD_RCS_EXHAUST(th_rcs[10], _V(8.5, 0.45, -10.693), _V(0, -1, 0));  // right side top : ---->>> Rear Top set: Pitch UP / translation down / Bank Right
    ADD_RCS_EXHAUST(th_rcs[10], _V(8.5, 0.45, -11.077), _V(0, -1, 0));

    // put the RCS directly on the Y centerline so we don't induce any rotation
    th_rcs[12] = CreateThruster (_V(0, 0,-rcsZHullDistance), _V(0, 0, 1), GetRCSThrustMax(12), ph_rcs, mainISP);   // aft
    th_rcs[13] = CreateThruster (_V(0, 0, rcsZHullDistance), _V(0, 0,-1), GetRCSThrustMax(13), ph_rcs, mainISP);   // fore

    // Translation exhausts
    // aft Z axis : ---->>> Rear set: Translation forward
    ADD_LARGE_RCS_EXHAUST(th_rcs[12], _V( 4.25,  0.25, TAIL_RCS_DCOORD(-11.8, -1)), _V(0, 0, -1));   
    ADD_LARGE_RCS_EXHAUST(th_rcs[12], _V(4.25, -0.25, TAIL_RCS_DCOORD(-11.8, -1)), _V(0, 0, -1));
    ADD_LARGE_RCS_EXHAUST(th_rcs[12], _V(-4.25, 0.25, TAIL_RCS_DCOORD(-11.8, -1)), _V(0, 0, -1));
    ADD_LARGE_RCS_EXHAUST(th_rcs[12], _V(-4.25, -0.25, TAIL_RCS_DCOORD(-11.8, -1)), _V(0, 0, -1));

    // fore Z axis : ---->>> Front set: Translation back
    ADD_LARGE_RCS_EXHAUST(th_rcs[13], _V(0.4, 0.915, RCS_DCOORD(20.66, 1)), _V(0, 0, 1));
    ADD_LARGE_RCS_EXHAUST(th_rcs[13], _V(0.0, 0.915, RCS_DCOORD(20.66, 1)), _V(0, 0, 1));
    ADD_LARGE_RCS_EXHAUST(th_rcs[13], _V(-0.4, 0.915, RCS_DCOORD(20.66, 1)), _V(0, 0, 1));

    // NOTE: must invoke ConfigureRCSJets later after the scenario file is read

    // **************** scramjet definitions ********************
    
    const double scramX = 1.0;  // distance from centerline
    for (int i = 0; i < 2; i++) 
    {
        th_scram[i] = CreateThruster (_V((i ? scramX : -scramX), 0, -rcsZHullDistance), _V(0, 0, 1), 0, ph_scram, 0);
        ramjet->AddThrusterDefinition (th_scram[i], SCRAM_FHV[GetXR1Config()->SCRAMfhv],
            SCRAM_INTAKE_AREA, SCRAM_INTERNAL_TEMAX, GetXR1Config()->GetScramMaxEffectiveDMF());
    }
    
    // thrust rating and ISP for scramjet engines are updated continuously
    PSTREAM_HANDLE ph;
    const double scramDelta = -1.0;   // move particles back from the engines slightly
    // Note: ph will be null if exhaust streams are disabled

    // XR3TODO: test this visually once the mesh is animated 
    const double scramY = 1.54;
    ph = AddExhaustStream(th_scram[0], _V(-scramX, -1.54, -9.0 + scramDelta), &exhaust_scram);
    if (ph != nullptr)
        oapiParticleSetLevelRef (ph, scram_intensity+0);

    ph = AddExhaustStream (th_scram[1], _V(scramX, -1.54, -9.0 + scramDelta), &exhaust_scram);
    if (ph != nullptr)
        oapiParticleSetLevelRef (ph, scram_intensity+1);
    
#ifdef WONT_WORK
    // ********************* aerodynamics ***********************

    // NOTE: org values were causing nasty downward pitch in the atmospehere: 
    // hwing = CreateAirfoil3 (LIFT_VERTICAL, _V(0,0,-0.3), VLiftCoeff, NULL, 5, 90, 1.5);
    // Note: aspect ratio = total span squared divided by wing area: 76.67^2 / mesh_wing_area
    const double aspectRatio = (54.909154*54.909154) / 479.07;
    hwing = CreateAirfoil3 (LIFT_VERTICAL, _V(m_wingBalance,0,m_centerOfLift), VLiftCoeff, NULL, 27.68, WING_AREA, aspectRatio);
    
    // vertical stabiliser and body lift and drag components
    // location: effectively in center of vessel (because there are two), and just over 20 meters back from the center of the vessel
    // Note: aspect ratio = total span squared divided by wing area: 16.31.16.31^2 / mesh_wing_area
    const double verticalStabilWingArea = 96.68;
    const double vertAspectRatio = (16.30805*16.30805) / verticalStabilWingArea;
    CreateAirfoil3 (LIFT_HORIZONTAL, _V(0,0,-20.12), HLiftCoeff, NULL, 16.797632, verticalStabilWingArea, vertAspectRatio);
    
    
    // ref vector is 1 meter behind the vertical stabilizers
    hElevator = CreateControlSurface2(AIRCTRL_ELEVATOR,     40.32, 1.4, _V(   0,0,-21.2), AIRCTRL_AXIS_XPOS, anim_elevator);

    CreateControlSurface (AIRCTRL_RUDDER,      96.68, 1.5, _V(   0,0,-21.2), AIRCTRL_AXIS_YPOS, anim_rudder);

    hLeftAileron = CreateControlSurface2 (AIRCTRL_AILERON, 15.38, 1.5, _V( 31.96,0,-21.2), AIRCTRL_AXIS_XPOS, anim_raileron);
    hRightAileron = CreateControlSurface2 (AIRCTRL_AILERON, 15.38, 1.5, _V(-31.96,0,-21.2), AIRCTRL_AXIS_XNEG, anim_laileron);
    
    CreateControlSurface(AIRCTRL_ELEVATORTRIM, 10.08, 1.5, _V(   0,0,-21.2), AIRCTRL_AXIS_XPOS, anim_elevatortrim);
#endif

    // ********************* aerodynamics ***********************
    
    // NOTE: org values were causing nasty downward pitch in the atmospehere: 
    // hwing = CreateAirfoil3 (LIFT_VERTICAL, _V(0,0,-0.3), VLiftCoeff, NULL, 5, 90, 1.5);
    m_ctrlSurfacesDeltaZ = -rcsZHullDistance;     // distance from center of model to center of control surfaces, Z axis
    m_aileronDeltaX = 13.0;      // distance from center of ship to center of aileron, X direction : this is appoximate, I don't have an exact number from Loru
    XR1Multiplier = XR1_MULTIPLIER;        // control surface area vs. the XR1 

    // center of lift matches center of mass
    // NOTE: this airfoil's force attack point will be modified by the SetCenterOfLift PreStep 
    hwing = CreateAirfoil3 (LIFT_VERTICAL, _V(m_wingBalance, 0, m_centerOfLift), VLiftCoeff, NULL, 5 * XR1Multiplier, WING_AREA, WING_ASPECT_RATIO);  
    
    CreateAirfoil3 (LIFT_HORIZONTAL, _V(0, 0, m_ctrlSurfacesDeltaZ + 3.0), HLiftCoeff, NULL, 16.79, 15 * XR1Multiplier, 1.5);

    ReinitializeDamageableControlSurfaces();  // create ailerons, elevators, and elevator trim

    // vertical stabiliser and body lift and drag components
    CreateControlSurface(AIRCTRL_RUDDER,       0.8 * XR1Multiplier,     1.5, _V(   0,0, m_ctrlSurfacesDeltaZ), AIRCTRL_AXIS_YPOS, anim_rudder);

    // Create a hidden elevator trim to fix the nose-up tendency on liftoff and allow the elevator trim to be truly neutral.
    // We have to use FLAP here because that is the only unused control surface type.  We could probably also duplicate this via CreateAirfoil3, but this
    // is easier to adjust and test.
    // XR3TODO: tweak this as necessary to fix nose-up push
    CreateControlSurface(AIRCTRL_FLAP, 0.3 * XR1Multiplier * 7, 1.5, _V(   0,0, m_ctrlSurfacesDeltaZ), AIRCTRL_AXIS_XPOS);  // no animation for this!
    m_hiddenElevatorTrimState = HIDDEN_ELEVATOR_TRIM_STATE;        // set to a member variable in case we want to change it in flight later
    // Note: cannot set the level here; it is reset by Orbiter later.
    
    const double xr1VariableDragModifier = XR1_MULTIPLIER;    // this is the empty mass ratio of the XR3:XR1
    // XR3TODO:  tweak these drag element coordinates
	// we need these goofy const casts to force the linker to link with the 'const double *' version of CreateVariableDragElement instead of the legacy 'double *' version of the function (which still exists but is deprecated and causes warnings in orbiter.log)
	CreateVariableDragElement(const_cast<const double *>(&rcover_proc),   0.2 * xr1VariableDragModifier, _V(0,  0.0, 26.972));     // retro covers
    CreateVariableDragElement(const_cast<const double *>(&radiator_proc), 0.4 * xr1VariableDragModifier, _V(0, 3.274, -rcsZHullDistance+5));     // radiators
    CreateVariableDragElement(const_cast<const double *>(&bay_proc),      7.0 * xr1VariableDragModifier, _V(0, 8.01, -rcsZHullDistance+8));       // bay doors (drag is at rear of bay)
    CreateVariableDragElement(const_cast<const double *>(&gear_proc),     0.8 * xr1VariableDragModifier, _V(0, -4,  4.34));          // landing gear
    CreateVariableDragElement(const_cast<const double *>(&nose_proc),     2.1 * xr1VariableDragModifier, _V(0, 3.06, 8.6));          // docking port
    CreateVariableDragElement(const_cast<const double *>(&brake_proc),    4.0 * xr1VariableDragModifier, _V(0, 0, m_ctrlSurfacesDeltaZ));  // airbrake (do not induce a rotational moment here)
    // XR3TODO: convert crewElevator_proc and associated code into ladder to the ground: CreateVariableDragElement(&crewElevator_proc, 6.0 * xr1VariableDragModifier, _V(-3.358, -6.51, 6.371));  // elevator (note: elevator is off-center)
    
    const double dragMultiplier = XR1_MULTIPLIER;
    SetRotDrag (_V(0.10 * dragMultiplier, 0.13 * dragMultiplier, 0.04 * dragMultiplier));

    // define hull temperature limits (these match the XR1's limits for now)
    m_hullTemperatureLimits.noseCone  = CTOK(2840);
    m_hullTemperatureLimits.wings     = CTOK(2380);
    m_hullTemperatureLimits.cockpit   = CTOK(1490);
    m_hullTemperatureLimits.topHull   = CTOK(1210);
    m_hullTemperatureLimits.warningFrac  = 0.80;   // yellow text
    m_hullTemperatureLimits.criticalFrac = 0.90;   // red text
    m_hullTemperatureLimits.doorOpenWarning = 0.75;
    // aluminum melts @ 660C and begins deforming below that
    m_hullTemperatureLimits.doorOpen = CTOK(480);

    // default to full LOX tank if not loaded from save file 
    if (m_loxQty < 0)
        m_loxQty = GetXR1Config()->GetMaxLoxMass();

    // ************************* mesh ***************************
    
    // ********************* beacon lights **********************
    const double bd = 0.4;  // beacon delta from the mesh edge
    // TODO: get these beacon coordinates from Loru
    static VECTOR3 beaconpos[7] = {
        {-37.605, 0.561 + bd, -18.939 + bd}, {37.605, 0.561 + bd, -18.939 + bd}, {0, 3.241, -30.489 - bd},  // nav: left wing, right wing, aft center
        {0, 7.958 + bd, 8.849}, {0, -1.26 - bd, 8.823},                     // beacon: top hull, bottom hull
        {-37.605, 7.932 + bd, -28.304}, {37.605, 7.932 + bd, -28.304},      // strobe: left rudder top, right rudder top
    };
    
    // RGB colors
    static VECTOR3 beaconcol[7] = {
        {1.0,0.5,0.5}, {0.5,1.0,0.5}, {1,1,1},  // nav RGB colors
        {1,0.6,0.6}, {1,0.6,0.6},               // beacon
        {1,1,1}, {1,1,1}                        // strobe
    };  

    const double sizeMultiplier = 1.5;  // XR3TODO: tweak this beacon size
    for (int i = 0; i < 7; i++) 
    {  
        beacon[i].shape = (i < 3 ? BEACONSHAPE_DIFFUSE : BEACONSHAPE_STAR);
        beacon[i].pos = beaconpos+i;
        beacon[i].col = beaconcol+i;
        beacon[i].size = (i < 3 ? 0.3 * sizeMultiplier : 0.55 * sizeMultiplier);
        beacon[i].falloff = (i < 3 ? 0.4 : 0.6);
        beacon[i].period = (i < 3 ? 0 : i < 5 ? 2 : 1.13);
        beacon[i].duration = (i < 5 ? 0.1 : 0.05);
        beacon[i].tofs = (6-i)*0.2;
        beacon[i].active = false;
        AddBeacon (beacon+i);
    }

    // light colors
    COLOUR4 col_d     = { 0.9f, 0.8f, 1.0f, 0.0f };  // diffuse
	COLOUR4 col_s     = { 1.9f, 0.8f, 1.0f ,0.0f };  // specular
	COLOUR4 col_a     = { 0.0f, 0.0f, 0.0f ,0.0f };  // ambient (black)
	COLOUR4 col_white = { 1.0f, 1.0f, 1.0f, 0.0f };  // white

    // add a light at each main engine set of 3
    const double mainEnginePointLightPower = 100 * 5.94;  // XR3 engines are 5.94 times as powerful as the XR1's
    const double zMainLightDelta = -3.0;  // need more delta here because the exhaust is sunk into the engine bell  XR3TODO: tweak this value
    if (GetXR1Config()->EnableEngineLightingEffects)
    {
	    LightEmitter *leMainPort      = AddPointLight(_V(-4.1095, 2.871, mainExhaustZCoord + zMainLightDelta), mainEnginePointLightPower, 1e-3, 0, 2e-3, col_d, col_s, col_a);
        LightEmitter *leMainStarboard = AddPointLight(_V( 4.1095, 2.871, mainExhaustZCoord + zMainLightDelta), mainEnginePointLightPower, 1e-3, 0, 2e-3, col_d, col_s, col_a);
	    leMainPort->SetIntensityRef(&m_mainThrusterLightLevel);
        leMainStarboard->SetIntensityRef(&m_mainThrusterLightLevel);
    }

    // add a light at each set of hover engines
    if (GetXR1Config()->EnableEngineLightingEffects)
    {
        const double hoverEnginePointLightPower = mainEnginePointLightPower * 0.7567;  // hovers are .7567 the thrust of the mains (different engine count notwithstanding)
        const double yHoverLightDelta = -1.0;
        LightEmitter *leForward      = AddPointLight (_V(  0.000, -1.460 + yHoverLightDelta,  12.799), hoverEnginePointLightPower, 1e-3, 0, 2e-3, col_d, col_s, col_a); 
        LightEmitter *leAftPort      = AddPointLight (_V(-22.324, -1.091 + yHoverLightDelta, -15.633), hoverEnginePointLightPower, 1e-3, 0, 2e-3, col_d, col_s, col_a); 
        LightEmitter *leAftStarboard = AddPointLight (_V( 22.324, -1.091 + yHoverLightDelta, -15.633), hoverEnginePointLightPower, 1e-3, 0, 2e-3, col_d, col_s, col_a); 
	    leForward->SetIntensityRef(&m_hoverThrusterLightLevel);
        leAftPort->SetIntensityRef(&m_hoverThrusterLightLevel);
        leAftStarboard->SetIntensityRef(&m_hoverThrusterLightLevel);
    }

    // add docking lights (2 forward and 2 docking)
    // Note: XR1/XR2 rage was 150 meters
    // forward
    // XR3TODO: get these two sets of landing light coordinates from Loru (these are on the wings)
    m_pSpotlights[0] = static_cast<SpotLight *>(AddSpotLight(_V( 10.628, -0.055, 3.586), _V(0,0,1), 250, 1e-3, 0, 1e-3, RAD*25, RAD*60, col_white, col_white, col_a));
    m_pSpotlights[1] = static_cast<SpotLight *>(AddSpotLight(_V(-10.628, -0.055, 3.586), _V(0,0,1), 250, 1e-3, 0, 1e-3, RAD*25, RAD*60, col_white, col_white, col_a));
    // docking port 
    // XR3TODO: tweak the Y coordinate here so the lights are against the hull
    m_pSpotlights[2] = static_cast<SpotLight *>(AddSpotLight(_V(-1.66, 3.060, 8.60), _V(0, 1, 0), 250, 1e-3, 0, 1e-3, RAD * 25, RAD * 60, col_white, col_white, col_a));
    m_pSpotlights[3] = static_cast<SpotLight *>(AddSpotLight(_V( 1.66, 3.060, 8.60), _V(0, 1 ,0), 250, 1e-3, 0, 1e-3, RAD*25, RAD*60, col_white, col_white, col_a));

    // turn all spotlights off by default
    for (int i=0; i < SPOTLIGHT_COUNT; i++)
        m_pSpotlights[i]->Activate(false);

    // load meshes
    // no VC: vcmesh_tpl = oapiLoadMeshGlobal("dg-xr1\\deltaglidercockpit-xr1");  // VC mesh
    vcmesh_tpl = nullptr;  // no VC; must set this to null so the superclass won't try to use it
    exmesh_tpl = oapiLoadMeshGlobal("XR3Phoenix\\XR3Phoenix");         // exterior mesh

    m_exteriorMeshIndex = AddMesh(exmesh_tpl);  // save so we can modify the mesh later
    SetMeshVisibilityMode(m_exteriorMeshIndex, MESHVIS_EXTERNAL);    
    // no VC: SetMeshVisibilityMode(AddMesh(vcmesh_tpl), MESHVIS_VC);
    
    ///////////////////////////////////////////////////////////////////////
    // Init UMmu
    ///////////////////////////////////////////////////////////////////////
#ifdef MMU
    const int ummuStatus = UMmu.InitUmmu(GetHandle());  // returns 1 if ok and other number if not

    // UMmu is REQUIRED!
    if (ummuStatus != 1)
        FatalError("UMmu not installed!  You must install Universal Mmu 3.0 or newer in order to use the XR3; visit http://www.alteaaerospace.com for more information.");

    // validate UMmu version and write it to the log file
    const float ummuVersion = CONST_UMMU_XR3(this).GetUserUMmuVersion();
    if (ummuVersion < 2.0)
    {
        char msg[256];
        sprintf(msg, "UMmu version %.2f is installed, but the XR3 requires Universal Mmu 3.0 or higher; visit http://www.alteaaerospace.com for more information.", ummuVersion);
        FatalError(msg);
    }

    char msg[256];
    sprintf(msg, "Using UMmu Version: %.2f", ummuVersion);
    GetXR1Config()->WriteLog(msg);
#endif
    // UMMU Bug: must invoke SetMaxSeatAvailableInShip and SetCrewWeightUpdateShipWeightAutomatically each time we redefine the airlock
    // NOTE: UMmu airlock definition and default crew data will be set again later AFTER the scenario file is parsed.
    DefineMmuAirlock();  // required here so that UMMu loads the crew from the scenario file!

    //
    // Initialize and cache all instrument panels
    //

    // 1920-pixel-wide panels
    AddInstrumentPanel(new XR3MainInstrumentPanel1920 (*this), 1920);
    AddInstrumentPanel(new XR3UpperInstrumentPanel1920(*this), 1920);
    AddInstrumentPanel(new XR3LowerInstrumentPanel1920(*this), 1920);
    AddInstrumentPanel(new XR3OverheadInstrumentPanel1920(*this), 1920);
    AddInstrumentPanel(new XR3PayloadInstrumentPanel1920(*this), 1920);

    // 1600-pixel-wide panels
    AddInstrumentPanel(new XR3MainInstrumentPanel1600 (*this), 1600);
    AddInstrumentPanel(new XR3UpperInstrumentPanel1600(*this), 1600);
    AddInstrumentPanel(new XR3LowerInstrumentPanel1600(*this), 1600);
    AddInstrumentPanel(new XR3OverheadInstrumentPanel1600(*this), 1600);
    AddInstrumentPanel(new XR3PayloadInstrumentPanel1600(*this), 1600);

    // 1280-pixel-wide panels
    AddInstrumentPanel(new XR3MainInstrumentPanel1280 (*this), 1280);
    AddInstrumentPanel(new XR3UpperInstrumentPanel1280(*this), 1280);
    AddInstrumentPanel(new XR3LowerInstrumentPanel1280(*this), 1280);
    AddInstrumentPanel(new XR3OverheadInstrumentPanel1280(*this), 1280);
    AddInstrumentPanel(new XR3PayloadInstrumentPanel1280(*this), 1280);

    // XR3TODO: uncomment this for VC
    // no VC yet for the XR3
#ifdef UNDEF
    // add our VC panels
    AddInstrumentPanel(new VCPilotInstrumentPanel     (*this, PANELVC_PILOT), 0);
    AddInstrumentPanel(new VCPassenger1InstrumentPanel(*this, PANELVC_PSNGR1), 0);
    AddInstrumentPanel(new VCPassenger2InstrumentPanel(*this, PANELVC_PSNGR2), 0);
    AddInstrumentPanel(new VCPassenger3InstrumentPanel(*this, PANELVC_PSNGR3), 0);
    AddInstrumentPanel(new VCPassenger4InstrumentPanel(*this, PANELVC_PSNGR4), 0);
#endif

}

// Create control surfaces for any damageable control surface handles below that are zero (all are zero before vessel initialized).
// This is invoked from clbkSetClassCaps as well as ResetDamageStatus.
void XR3Phoenix::ReinitializeDamageableControlSurfaces()
{
    if (hElevator == 0)
    {
        hElevator = CreateControlSurface2(AIRCTRL_ELEVATOR,     1.2 * XR1Multiplier * 3, 1.4, _V(   0,0, m_ctrlSurfacesDeltaZ), AIRCTRL_AXIS_XPOS, anim_elevator);
    }

    if (hLeftAileron == 0)
    {
        // ORG: hLeftAileron = CreateControlSurface2 (AIRCTRL_AILERON, 0.3, 1.5, _V( 7.5,0,-7.2), AIRCTRL_AXIS_XPOS, anim_raileron);
        hLeftAileron = CreateControlSurface2 (AIRCTRL_AILERON, 0.2 * XR1Multiplier * 2, 1.5, _V( m_aileronDeltaX, 0, m_ctrlSurfacesDeltaZ), AIRCTRL_AXIS_XPOS, anim_raileron);
    }

    if (hRightAileron == 0)
    {
        // ORG: hRightAileron = CreateControlSurface2 (AIRCTRL_AILERON, 0.3, 1.5, _V(-7.5,0,-7.2), AIRCTRL_AXIS_XNEG, anim_laileron);
        hRightAileron = CreateControlSurface2 (AIRCTRL_AILERON, 0.2 * XR1Multiplier * 2, 1.5, _V(-m_aileronDeltaX, 0, m_ctrlSurfacesDeltaZ), AIRCTRL_AXIS_XNEG, anim_laileron);
    }
    
    if (hElevatorTrim == 0)
    {
        // XR3 ORG: CreateControlSurface(AIRCTRL_ELEVATORTRIM, 0.3 * XR1Multiplier * 7, 1.5, _V(   0,0, controlSurfacesDeltaZ), AIRCTRL_AXIS_XPOS, anim_elevatortrim);
        // NOTE: increased this area to assist the autopilot in maintaining flight control in an atmosphere.
        hElevatorTrim = CreateControlSurface2 (AIRCTRL_ELEVATORTRIM, 0.3 * XR1Multiplier * 7, 1.5, _V(   0,0, m_ctrlSurfacesDeltaZ), AIRCTRL_AXIS_XPOS, anim_elevatortrim);
    }
}

// --------------------------------------------------------------
// Respond to playback event
// NOTE: do not use spaces in any of these event ID strings.
// --------------------------------------------------------------
bool XR3Phoenix::clbkPlaybackEvent (double simt, double event_t, const char *event_type, const char *event)
{
    // check for XR3-specific events
    // XR3TODO: convert crewElevator_proc and associated code into ladder to the ground
    if (!_stricmp (event_type, "ELEVATOR"))
    {
        ActivateElevator (!_stricmp (event, "CLOSE") ? DoorStatus::DOOR_CLOSING : DoorStatus::DOOR_OPENING);
        return true;
    } 

    // else let our superclass have it
    return DeltaGliderXR1::clbkPlaybackEvent(simt, event_t, event_type, event);
}

// --------------------------------------------------------------
// Finalize vessel creation
// --------------------------------------------------------------
void XR3Phoenix::clbkPostCreation()
{
    // Invoke XR PostCreation code common to all XR vessels (code is in XRVessel.cpp)
    clbkPostCreationCommonXRCode();

    // configure RCS thruster groups and override the max thrust values if necessary
    ConfigureRCSJets(m_rcsDockingMode);    

    // Initialize XR payload vessel data
    XRPayloadClassData::InitializeXRPayloadClassData();

    DefineMmuAirlock();    // update UMmu airlock data based on current active EVA port

    EnableRetroThrusters(rcover_status == DoorStatus::DOOR_OPEN);
    EnableHoverEngines(hoverdoor_status == DoorStatus::DOOR_OPEN);
    EnableScramEngines(scramdoor_status == DoorStatus::DOOR_OPEN);

    // set initial animation states
    SetXRAnimation(anim_gear,         gear_proc);
    SetXRAnimation(anim_rcover,       rcover_proc);
    SetXRAnimation(anim_hoverdoor,    hoverdoor_proc);
    SetXRAnimation(anim_scramdoor,    scramdoor_proc);
    SetXRAnimation(anim_nose,         nose_proc);
    SetXRAnimation(anim_ladder,       ladder_proc);
    SetXRAnimation(anim_olock,        olock_proc);
    SetXRAnimation(anim_ilock,        ilock_proc);
    SetXRAnimation(anim_hatch,        hatch_proc);
    SetXRAnimation(anim_radiator,     radiator_proc);
    SetXRAnimation(anim_brake,        brake_proc);
    SetXRAnimation(anim_bay,          bay_proc);
    // XR3TODO: convert crewElevator_proc and associated code into ladder to the ground: SetXRAnimation(anim_crewElevator, crewElevator_proc);
    
    // NOTE: instrument panel initialization moved to clbkSetClassCaps (earlier) because the Post-2010-P1 Orbiter Beta invokes clbkLoadPanel before invoking clbkPostCreation

    // add our PreStep objects; these are invoked in order
    AddPreStep(new DrainBayFuelTanksPreStep(*this));  // need to do this *first* so the gauges are all correct later in the timestep (keeps main/SCRAM tanks full)
    AddPreStep(new RefreshSlotStatesPreStep(*this));  // do this early in case any other presteps look at the slot state
    AddPreStep(new AttitudeHoldPreStep     (*this));         
    AddPreStep(new DescentHoldPreStep      (*this));
    AddPreStep(new AirspeedHoldPreStep     (*this));
    AddPreStep(new ScramjetSoundPreStep    (*this));
    AddPreStep(new MmuPreStep              (*this));
    AddPreStep(new GearCalloutsPreStep     (*this));
    AddPreStep(new MachCalloutsPreStep     (*this));
    AddPreStep(new AltitudeCalloutsPreStep (*this));
    AddPreStep(new DockingCalloutsPreStep  (*this));
    AddPreStep(new TakeoffAndLandingCalloutsAndCrashPreStep(*this));
    AddPreStep(new AnimateGearCompressionPreStep           (*this));
    AddPreStep(new RotateWheelsPreStep                     (*this));  // NOTE: this must be *after* AnimateGearCompressionPreStep so that we can detect whether the wheels are touching the ground or not for this timestep
    AddPreStep(new XR3NosewheelSteeringPreStep             (*this));  // NOTE: this must be *after* AnimateGearCompressionPreStep so that we can detect whether the nosewheel is touching the ground or not for this timestep
    AddPreStep(new RefreshGrappleTargetsInDisplayRangePreStep     (*this));
    AddPreStep(new UpdateVesselLightsPreStep(*this));
	AddPreStep(new ParkingBrakePreStep      (*this));

    // WARNING: this must be invoked LAST in the sequence so that behavior is consistent across all pre-step methods
    AddPreStep(new UpdatePreviousFieldsPreStep   (*this));

    // add our PostStep objects; these are invoked in order
    AddPostStep(new PreventAutoRefuelPostStep   (*this));   // add this FIRST before our fuel callouts
    AddPostStep(new ComputeAccPostStep          (*this));   // used by acc areas; computed only once per frame for efficiency
    // XRSound: AddPostStep(new AmbientSoundsPostStep       (*this));
    AddPostStep(new ShowWarningPostStep         (*this));
    AddPostStep(new SetHullTempsPostStep        (*this));
    AddPostStep(new SetSlopePostStep            (*this));
    // do not include DoorSoundsPostStep here; we replace it below
    AddPostStep(new FuelCalloutsPostStep        (*this));
    AddPostStep(new UpdateIntervalTimersPostStep(*this));
    AddPostStep(new APUPostStep                 (*this));
    AddPostStep(new UpdateMassPostStep          (*this));
    AddPostStep(new DisableControlSurfForAPUPostStep    (*this));
	AddPostStep(new OneShotInitializationPostStep(*this));
    AddPostStep(new AnimationPostStep           (*this));
    AddPostStep(new FuelDumpPostStep            (*this));
    AddPostStep(new XFeedPostStep               (*this));
    AddPostStep(new ResupplyPostStep            (*this));
    AddPostStep(new LOXConsumptionPostStep      (*this));
    AddPostStep(new UpdateCoolantTempPostStep   (*this));
    AddPostStep(new AirlockDecompressionPostStep(*this));
    AddPostStep(new AutoCenteringSimpleButtonAreasPostStep(*this));  // logic for all auto-centering button areas
    AddPostStep(new ResetAPUTimerForPolledSystemsPostStep (*this));
    AddPostStep(new ManageMWSPostStep                     (*this));

    // NEW poststeps specific to the XR3
    AddPostStep(new SwitchTwoDPanelPostStep                  (*this));
    AddPostStep(new XR3AnimationPostStep                     (*this));
    AddPostStep(new XR3DoorSoundsPostStep                    (*this));  // replaces the standard DoorSoundsPostStep in the XR1 class
    AddPostStep(new HandleDockChangesForActiveAirlockPostStep(*this));  // switch active airlock automatically as necessary

#ifdef _DEBUG
    AddPostStep(new TestXRVesselCtrlPostStep    (*this));      // for manual testing of new XRVesselCtrl methods via the debugger
#endif

    // set hidden elevator trim level
    SetControlSurfaceLevel(AIRCTRL_FLAP, m_hiddenElevatorTrimState);  
}

// --------------------------------------------------------------
// Create visual
// --------------------------------------------------------------
void XR3Phoenix::clbkVisualCreated (VISHANDLE vis, int refcount)
{
    exmesh = GetDevMesh (vis, 0);
    // no VC: vcmesh = GetMesh (vis, 1);
    vcmesh = nullptr;
    SetPassengerVisuals();    // NOP for now, but let's invoke it anyway
    SetDamageVisuals();
    
    ApplySkin();
    
    // no VC: UpdateVCStatusIndicators();
    
    // redraw the navmode buttons
    TriggerNavButtonRedraw();

    // no VC: UpdateVCMesh();

    // show or hide the landing gear
    SetGearParameters(gear_proc);
}

// Invoked whenever the crew onboard change
void XR3Phoenix::SetPassengerVisuals()
{
    // nothing to do
}

// --------------------------------------------------------------
// Destroy visual
// --------------------------------------------------------------
void XR3Phoenix::clbkVisualDestroyed (VISHANDLE vis, int refcount)
{
    exmesh = nullptr;
    vcmesh = nullptr;
}


// Superclass' clbkPreStep and clbkPostStep are all we need

// ==============================================================
// Message callback function for control dialog box
// ==============================================================

INT_PTR CALLBACK XR3Ctrl_DlgProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    XR3Phoenix *dg = (uMsg == WM_INITDIALOG ? reinterpret_cast<XR3Phoenix *>(lParam) : reinterpret_cast<XR3Phoenix *>(oapiGetDialogContext(hWnd)));
    // pointer to vessel instance was passed as dialog context
    
    switch (uMsg) {
    /* Note: for some reason Orbiter appears to be trapping keystrokes, so this will not work.
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            oapiCloseDialog(hWnd);   // bye, bye
        break;  // pass it on
    */

    case WM_INITDIALOG:
        dg->UpdateCtrlDialog(dg, hWnd);
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDCANCEL:
            oapiCloseDialog (hWnd);
            return TRUE;

        case IDC_GEAR_UP:
            dg->ActivateLandingGear(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_GEAR_DOWN:
            dg->ActivateLandingGear(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_RETRO_CLOSE:
            dg->ActivateRCover(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_RETRO_OPEN:
            dg->ActivateRCover(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_BAY_CLOSE:
            dg->ActivateBayDoors(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_BAY_OPEN:
            dg->ActivateBayDoors(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_OLOCK_CLOSE:
            dg->ActivateOuterAirlock(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_OLOCK_OPEN:
            dg->ActivateOuterAirlock(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_ILOCK_CLOSE:
            dg->ActivateInnerAirlock(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_ILOCK_OPEN:
            dg->ActivateInnerAirlock(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_HOVER_CLOSE:
            dg->ActivateHoverDoors(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_HOVER_OPEN:
            dg->ActivateHoverDoors(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_DOCKING_STOW:
            dg->ActivateNoseCone(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_DOCKING_DEPLOY:
            dg->ActivateNoseCone(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_ELEVATOR_STOW:
            dg->ActivateElevator(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_ELEVATOR_DEPLOY:
            dg->ActivateElevator(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_SCRAM_CLOSE:
            dg->ActivateScramDoors(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_SCRAM_OPEN:
            dg->ActivateScramDoors(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_HATCH_CLOSE:
            dg->ActivateHatch(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_HATCH_OPEN:
            dg->ActivateHatch(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_RADIATOR_STOW:
            dg->ActivateRadiator(DoorStatus::DOOR_CLOSING);
            return 0;
        case IDC_RADIATOR_DEPLOY:
            dg->ActivateRadiator(DoorStatus::DOOR_OPENING);
            return 0;

        case IDC_NAVLIGHT:
            dg->SetNavlight (SendDlgItemMessage(hWnd, IDC_NAVLIGHT, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_BEACONLIGHT:
            dg->SetBeacon (SendDlgItemMessage(hWnd, IDC_BEACONLIGHT, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_STROBELIGHT:
            dg->SetStrobe (SendDlgItemMessage(hWnd, IDC_STROBELIGHT, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        }
        break;
    }
    return oapiDefDialogProc(hWnd, uMsg, wParam, lParam);
}

void XR3Phoenix::UpdateCtrlDialog(XR3Phoenix *dg, HWND hWnd)
{
    static int bstatus[2] = {BST_UNCHECKED, BST_CHECKED};
    
    if (hWnd == nullptr) 
        hWnd = oapiFindDialog (g_hDLL, IDD_CTRL);

    if (hWnd == nullptr) 
        return;

    int op;
    
    op = static_cast<int>(dg->gear_status) & 1;
    SendDlgItemMessage (hWnd, IDC_GEAR_DOWN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_GEAR_UP, BM_SETCHECK, bstatus[1-op], 0);
    
    op = static_cast<int>(dg->rcover_status) & 1;
    SendDlgItemMessage (hWnd, IDC_RETRO_OPEN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_RETRO_CLOSE, BM_SETCHECK, bstatus[1-op], 0);
    
    op = static_cast<int>(dg->bay_status) & 1;
    SendDlgItemMessage (hWnd, IDC_BAY_OPEN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_BAY_CLOSE, BM_SETCHECK, bstatus[1-op], 0);

    op = static_cast<int>(dg->olock_status) & 1;
    SendDlgItemMessage (hWnd, IDC_OLOCK_OPEN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_OLOCK_CLOSE, BM_SETCHECK, bstatus[1-op], 0);
    
    op = static_cast<int>(dg->ilock_status) & 1;
    SendDlgItemMessage (hWnd, IDC_ILOCK_OPEN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_ILOCK_CLOSE, BM_SETCHECK, bstatus[1-op], 0);

    op = static_cast<int>(dg->hoverdoor_status) & 1;
    SendDlgItemMessage (hWnd, IDC_HOVER_OPEN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_HOVER_CLOSE, BM_SETCHECK, bstatus[1-op], 0);

    op = static_cast<int>(dg->nose_status) & 1;
    SendDlgItemMessage (hWnd, IDC_DOCKING_DEPLOY, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_DOCKING_STOW, BM_SETCHECK, bstatus[1-op], 0);
    
    op = static_cast<int>(dg->crewElevator_status) & 1;
    SendDlgItemMessage (hWnd, IDC_ELEVATOR_DEPLOY, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_ELEVATOR_STOW, BM_SETCHECK, bstatus[1-op], 0);
    
    op = static_cast<int>(dg->scramdoor_status) & 1;
    SendDlgItemMessage (hWnd, IDC_SCRAM_OPEN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_SCRAM_CLOSE, BM_SETCHECK, bstatus[1-op], 0);

    op = static_cast<int>(dg->hatch_status) & 1;
    SendDlgItemMessage (hWnd, IDC_HATCH_OPEN, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_HATCH_CLOSE, BM_SETCHECK, bstatus[1-op], 0);
    
    op = static_cast<int>(dg->radiator_status) & 1;
    SendDlgItemMessage (hWnd, IDC_RADIATOR_DEPLOY, BM_SETCHECK, bstatus[op], 0);
    SendDlgItemMessage (hWnd, IDC_RADIATOR_STOW, BM_SETCHECK, bstatus[1-op], 0);
    
    op = static_cast<int>(dg->beacon[0].active) ? 1:0;
    SendDlgItemMessage (hWnd, IDC_NAVLIGHT, BM_SETCHECK, bstatus[op], 0);
    op = static_cast<int>(dg->beacon[3].active) ? 1:0;
    SendDlgItemMessage (hWnd, IDC_BEACONLIGHT, BM_SETCHECK, bstatus[op], 0);
    op = static_cast<int>(dg->beacon[5].active) ? 1:0;
    SendDlgItemMessage (hWnd, IDC_STROBELIGHT, BM_SETCHECK, bstatus[op], 0);
}

// toggle RCS docking mode
// rcsMode: true = set docking mode, false = set normal mode
// Returns: true if mode switched successfully, false if mode switch was inhibited
bool XR3Phoenix::SetRCSDockingMode(bool dockingMode)
{
    // if enabling docking mode and any autopilot is engaged, prohibit the change
    bool autopilotEngaged = false;
    if (dockingMode)
    {
        // check whether any standard autopilot is engaged
        for (int i=1; i <= 7; i++)
        {
            if (GetNavmodeState(i))
            {
                autopilotEngaged = true;   
                break;
            }
        }

        // check for any custom autopilot except for Airspeed Hold
        autopilotEngaged |= (m_customAutopilotMode != AUTOPILOT::AP_OFF);

        if (autopilotEngaged)
        {
            PlayErrorBeep();
            ShowWarning("RCS locked by Autopilot.wav", DeltaGliderXR1::ST_WarningCallout, "Autopilot is active: RCS mode is locked.");
            return false;
        }
    }

    ConfigureRCSJets(dockingMode);     
    PlaySound((dockingMode ? BeepHigh : BeepLow), DeltaGliderXR1::ST_Other);

    // play voice callout
    if (dockingMode)
        ShowInfo("RCS Config Docking.wav", DeltaGliderXR1::ST_InformationCallout, "RCS jets set to DOCKING configuration.");
    else
        ShowInfo("RCS Config Normal.wav", DeltaGliderXR1::ST_InformationCallout, "RCS jets set to NORMAL configuration.");
    
    return true;
}

// Configure RCS jets for docking or normal mode by configuring RCS thruster groups.  
// This method does NOT display any message or play any sounds; however, it does redraw then RCS mode light/switch.
// rcsMode: true = set docking mode, false = set normal mode
void XR3Phoenix::ConfigureRCSJets(bool dockingMode)
{
    // delete any existing RCS thruster groups
    DelThrusterGroup(THGROUP_ATT_PITCHUP);
    DelThrusterGroup(THGROUP_ATT_PITCHDOWN);
    DelThrusterGroup(THGROUP_ATT_UP);
    DelThrusterGroup(THGROUP_ATT_DOWN);

    DelThrusterGroup(THGROUP_ATT_YAWLEFT);
    DelThrusterGroup(THGROUP_ATT_YAWRIGHT);
    DelThrusterGroup(THGROUP_ATT_LEFT);
    DelThrusterGroup(THGROUP_ATT_RIGHT);

    DelThrusterGroup(THGROUP_ATT_BANKLEFT);
    DelThrusterGroup(THGROUP_ATT_BANKRIGHT);

    DelThrusterGroup(THGROUP_ATT_FORWARD);
    DelThrusterGroup(THGROUP_ATT_BACK);

    THRUSTER_HANDLE th_att_rot[4], th_att_lin[4];

    if (dockingMode == false)   
    {
        // NORMAL mode
        th_att_rot[0] = th_att_lin[0] = th_rcs[0];  // fore up
        th_att_rot[1] = th_att_lin[3] = th_rcs[1];  // aft down
        th_att_rot[2] = th_att_lin[2] = th_rcs[2];  // fore down
        th_att_rot[3] = th_att_lin[1] = th_rcs[3];  // aft up
        CreateThrusterGroup (th_att_rot,   2, THGROUP_ATT_PITCHUP);   // rotate UP on X axis (+x)
        CreateThrusterGroup (th_att_rot+2, 2, THGROUP_ATT_PITCHDOWN); // rotate DOWN on X axis (-x)
        CreateThrusterGroup (th_att_lin,   2, THGROUP_ATT_UP);        // translate UP along Y axis (+y)
        CreateThrusterGroup (th_att_lin+2, 2, THGROUP_ATT_DOWN);      // translate DOWN along Y axis (-y)

        th_att_rot[0] = th_att_lin[0] = th_rcs[4];  // fore left
        th_att_rot[1] = th_att_lin[3] = th_rcs[5];  // aft right
        th_att_rot[2] = th_att_lin[2] = th_rcs[6];  // fore right
        th_att_rot[3] = th_att_lin[1] = th_rcs[7];  // aft left
        CreateThrusterGroup (th_att_rot,   2, THGROUP_ATT_YAWLEFT);   // rotate LEFT on Y axis (-y)
        CreateThrusterGroup (th_att_rot+2, 2, THGROUP_ATT_YAWRIGHT);  // rotate RIGHT on Y axis (+y)
        CreateThrusterGroup (th_att_lin,   2, THGROUP_ATT_LEFT);      // translate LEFT along X axis (-x)
        CreateThrusterGroup (th_att_lin+2, 2, THGROUP_ATT_RIGHT);     // translate RIGHT along X axis (+x)

        th_att_rot[0] = th_rcs[8];     // right wing bottom
        th_att_rot[1] = th_rcs[9];     // left wing top
        th_att_rot[2] = th_rcs[10];    // left wing bottom
        th_att_rot[3] = th_rcs[11];    // right wing top
        CreateThrusterGroup (th_att_rot,   2, THGROUP_ATT_BANKLEFT);  // rotate LEFT on Z axis (-Z)
        CreateThrusterGroup (th_att_rot+2, 2, THGROUP_ATT_BANKRIGHT); // rotate RIGHT on Z axis (+Z)

        th_att_lin[0] = th_rcs[12];   // aft
        th_att_lin[1] = th_rcs[13];   // fore
        CreateThrusterGroup (th_att_lin,   1, THGROUP_ATT_FORWARD);   // translate FORWARD along Z axis (+z)
        CreateThrusterGroup (th_att_lin+1, 1, THGROUP_ATT_BACK);      // translate BACKWARD along Z axis (-z)
    }
    else  // DOCKING mode
    {
        // For DOCKING mode, the Z and Y axes are exchanged:
        // X axis remains UNCHANGED
        // +Y = +Z
        // -Y = -Z
        // +Z = +Y
        // -Z = -Y
        th_att_rot[0] = th_att_lin[0] = th_rcs[0];  // fore up
        th_att_rot[1] = th_att_lin[3] = th_rcs[1];  // aft down
        th_att_rot[2] = th_att_lin[2] = th_rcs[2];  // fore down
        th_att_rot[3] = th_att_lin[1] = th_rcs[3];  // aft up
        CreateThrusterGroup (th_att_rot,   2, THGROUP_ATT_PITCHUP);   // rotate UP on X axis (+x)
        CreateThrusterGroup (th_att_rot+2, 2, THGROUP_ATT_PITCHDOWN); // rotate DOWN on X axis (-x)
        CreateThrusterGroup (th_att_lin,   2, THGROUP_ATT_FORWARD);   // old: translate UP along Y axis (+y) = new: +Z
        CreateThrusterGroup (th_att_lin+2, 2, THGROUP_ATT_BACK);      // old: translate DOWN along Y axis (-y) = new: -Z

        th_att_rot[0] = th_att_lin[0] = th_rcs[4];  // fore left
        th_att_rot[1] = th_att_lin[3] = th_rcs[5];  // aft right
        th_att_rot[2] = th_att_lin[2] = th_rcs[6];  // fore right
        th_att_rot[3] = th_att_lin[1] = th_rcs[7];  // aft left
        CreateThrusterGroup (th_att_rot,   2, THGROUP_ATT_BANKRIGHT);  // old: rotate LEFT on Y axis (-y) = new: -Z
        CreateThrusterGroup (th_att_rot+2, 2, THGROUP_ATT_BANKLEFT);   // old: rotate RIGHT on Y axis (+y) = new: +Z
        CreateThrusterGroup (th_att_lin,   2, THGROUP_ATT_LEFT);       // translate LEFT along X axis (-x)
        CreateThrusterGroup (th_att_lin+2, 2, THGROUP_ATT_RIGHT);      // translate RIGHT along X axis (+x)

        th_att_rot[0] = th_rcs[8];     // right wing bottom
        th_att_rot[1] = th_rcs[9];     // left wing top
        th_att_rot[2] = th_rcs[10];    // left wing bottom
        th_att_rot[3] = th_rcs[11];    // right wing top
        CreateThrusterGroup (th_att_rot,   2, THGROUP_ATT_YAWLEFT);  // old: rotate LEFT on Z axis (+Z) = new: -Y
        CreateThrusterGroup (th_att_rot+2, 2, THGROUP_ATT_YAWRIGHT); // old: rotate RIGHT on Z axis (-Z) = new: +Z

        th_att_lin[0] = th_rcs[12];   // aft
        th_att_lin[1] = th_rcs[13];   // fore
        CreateThrusterGroup (th_att_lin,   1, THGROUP_ATT_DOWN);   // old: translate FORWARD along Z axis (+z) = new: -Y  
        CreateThrusterGroup (th_att_lin+1, 1, THGROUP_ATT_UP);     // old: translate BACKWARD along Z axis (-z) = new: +Y
    }

    // reset all thruster levels
    // NOTE: must take damage into account here!
    double rcsThrusterPowerFrac = (dockingMode? 0.40 : 1.0);  // reduce thruster power in docking mode
    for (int i=0; i < 14; i++)
    {
        // get integrity fraction
        int damageIntegrityIndex = static_cast<int>(DamageItem::RCS1) + i;    // 0 <= i <= 13
        DamageStatus ds = GetDamageStatus((DamageItem)damageIntegrityIndex);
        SetThrusterMax0(th_rcs[i], (GetRCSThrustMax(i) * rcsThrusterPowerFrac * ds.fracIntegrity));  
    }

    m_rcsDockingMode = dockingMode;     
    TriggerRedrawArea(AID_RCS_CONFIG_BUTTON);
}

// hook this so we can disable docking mode automatically
void XR3Phoenix::SetCustomAutopilotMode(AUTOPILOT mode, bool playSound, bool force)
{
    if (mode != AUTOPILOT::AP_OFF)
        ConfigureRCSJets(false);    // revert to normal mode

    DeltaGliderXR1::SetCustomAutopilotMode(mode, playSound, force); // do the work
}

// set the active EVA port
void XR3Phoenix::SetActiveEVAPort(ACTIVE_EVA_PORT newState)
{
    m_activeEVAPort = newState;
    
    // update the UMMu port coordinates and repaint the LEDs and the switch
    DefineMmuAirlock();
}

// --------------------------------------------------------------
// Respond to navmode change
// NOTE: this does NOT include any custom autopilots such as 
// ATTITUDE HOLD and DESCENT HOLD.
// --------------------------------------------------------------
void XR3Phoenix::clbkNavMode(int mode, bool active)
{
    if (mode == NAVMODE_KILLROT)
    {
        if (active)
        {
            m_rcsDockingModeAtKillrotStart = m_rcsDockingMode;
            ConfigureRCSJets(false);            // must revert for killrot to work properly
        }
        else  // killrot just disengaged
        {
            ConfigureRCSJets(m_rcsDockingModeAtKillrotStart);    // restore previous state
        }
    }
    else    // some other mode: disable docking config if mode is active
    {
        if (active)
            ConfigureRCSJets(false);  // must revert for autopilots to work properly
    }

    // propogate to the superclass
    DeltaGliderXR1::clbkNavMode (mode, active);
}

// state: 0=fully retracted, 1.0 = fully deployed
void XR3Phoenix::SetGearParameters(double state)
{
    if (state == 1.0) // fully deployed?
    {
        const double touchdownDeltaX = 16.283;
        const double touchdownY = GEAR_UNCOMPRESSED_YCOORD + GEAR_COMPRESSION_DISTANCE; // gear height fully compressed

        SetXRTouchdownPoints(_V(               0, touchdownY, NOSE_GEAR_ZCOORD),    // front
                             _V(-touchdownDeltaX, touchdownY, REAR_GEAR_ZCOORD),    // left
                             _V( touchdownDeltaX, touchdownY, REAR_GEAR_ZCOORD),    // right
							 WHEEL_FRICTION_COEFF, WHEEL_LATERAL_COEFF, true);
        SetNosewheelSteering(true);  // not really necessary since we have have a prestep constantly checking this
    } 
    else  // not fully deployed (belly landing!)
    {
        const double touchdownDeltaX = 4.509;
        const double touchdownZRear = -17.754;

        SetXRTouchdownPoints(_V(               0, -1.248, 21.416),              // front
                             _V(-touchdownDeltaX, -3.666, touchdownZRear),      // left
                             _V( touchdownDeltaX, -3.150, touchdownZRear),      // right (tilt the ship)
							 3.0, 3.0, false);      // belly landing!
        SetNosewheelSteering(false);  // not really necessary since we have have a prestep constantly checking this
    }

    // update the animation state
    gear_proc = state;
    SetXRAnimation(anim_gear, gear_proc);

     // redraw the gear indicator
    TriggerRedrawArea(AID_GEARINDICATOR);

    // PERFORMANCE ENHANCEMENT: hide the gear if it is fully retracted; otherwise, render it
    static const UINT gearMeshGroups[] = 
    { 
        GRP_nose_oleo_piston, GRP_nose_axle_piston, GRP_nose_axle_cylinder, GRP_nose_axle, GRP_nose_oleo_piston, GRP_nose_gear_wheel_right, GRP_nose_gear_wheel_left,
        GRP_axle_left, GRP_axle_right, GRP_gear_main_oleo_cylinder_right, GRP_axle_piston_left, GRP_axle_cylinder_left, GRP_axle_cylinder_right, GRP_axle_piston_right, GRP_oleo_piston_right,
        GRP_oleo_piston_left, GRP_wheel_left_front_left_side, GRP_wheel_right_front_left_side, GRP_wheel_left_rear_left_side, GRP_wheel_right_rear_left_side, GRP_wheel_left_rear_right_side,
        GRP_wheel_right_rear_right_side, GRP_wheel_left_front_right_side, GRP_wheel_right_front_right_side, GRP_gear_main_oleo_cylinder_left, GRP_nose_oleo_cylinder 
    };

    SetMeshGroupsVisibility((state != 0.0), exmesh, SizeOfGrp(gearMeshGroups), gearMeshGroups);
}

// handle instant jumps to open or closed here
#define CHECK_DOOR_JUMP(proc, anim) if (action == DoorStatus::DOOR_OPEN) proc = 1.0;            \
                                    else if (action == DoorStatus::DOOR_CLOSED) proc = 0.0;     \
                                    SetXRAnimation(anim, proc)

// activate the bay doors (must override base class because of our radiator check)
void XR3Phoenix::ActivateBayDoors(DoorStatus action)
{
    // cannot deploy or retract bay doors if the radiator is in motion
    // NOTE: allow for DoorStatus::DOOR_FAILED here so that a radiator failure does not lock the bay doors
    if ((radiator_status == DoorStatus::DOOR_OPENING) || (radiator_status == DoorStatus::DOOR_CLOSING))
    {
        PlayErrorBeep();
        ShowWarning("Warning Radiator in Motion Bay Doors Are Locked.wav", DeltaGliderXR1::ST_WarningCallout, "Cannot open/close bay doors while&radiator is in motion.");
        return;  // cannot move
    }

    // OK to move doors as far as the radiator is concerned; invoke the base class
    DeltaGliderXR1::ActivateBayDoors(action);
}

// activate the crew elevator
void XR3Phoenix::ActivateElevator(DoorStatus action)
{
    // check for failure
    if (crewElevator_status == DoorStatus::DOOR_FAILED)
    {
        PlayErrorBeep();
        ShowWarning("Warning Elevator Failure.wav", DeltaGliderXR1::ST_WarningCallout, "Elevator inoperative due to excessive&heat and/or dynamic pressure.");
        return;  // cannot move
    }

    if (CheckHydraulicPressure(true, true) == false)   // show warning if no hydraulic pressure
        return;     // no hydraulic pressure

    // verify the gear has not collapsed!
    if (GetAltitude(ALTMODE_GROUND) < (GEAR_FULLY_COMPRESSED_DISTANCE-0.2))  // leave a 0.2-meter safety cushion
    {
        PlayErrorBeep();
        ShowWarning("Warning Elevator Failure.wav", DeltaGliderXR1::ST_WarningCallout, "Elevator inoperative: ground impact.");
        return;  // cannot move
    }

    bool close = (action == DoorStatus::DOOR_CLOSING) || (action == DoorStatus::DOOR_CLOSED);
    crewElevator_status = action;

    CHECK_DOOR_JUMP(crewElevator_proc, anim_crewElevator);

    TriggerRedrawArea(AID_ELEVATORSWITCH);
    TriggerRedrawArea(AID_ELEVATORINDICATOR);
    UpdateCtrlDialog(this);
    RecordEvent("ELEVATOR", close ? "CLOSE" : "OPEN");
}

// invoked from key handler
void XR3Phoenix::ToggleElevator()
{
    ActivateElevator(crewElevator_status == DoorStatus::DOOR_CLOSED || crewElevator_status == DoorStatus::DOOR_CLOSING ?
        DoorStatus::DOOR_OPENING : DoorStatus::DOOR_CLOSING);
}

// Override the base class method so we can perform some additional checks
void XR3Phoenix::ActivateRadiator(DoorStatus action)
{
    // check for failure
    if (radiator_status == DoorStatus::DOOR_FAILED)
    {
        PlayErrorBeep();
        ShowWarning("Warning Radiator Failure.wav", DeltaGliderXR1::ST_WarningCallout, "Radiator inoperative due to excessive&heat and/or dynamic pressure.");
        return;  // cannot move
    }

    if (CheckHydraulicPressure(true, true) == false)   // show warning if no hydraulic pressure
        return;     // no hydraulic pressure

    // cannot deploy or retract radiator if the payload bay doors are in motion
    // NOTE: allow for DoorStatus::DOOR_FAILED here so that a bay door failure does not lock the radiator
    if ((bay_status == DoorStatus::DOOR_OPENING) || (bay_status == DoorStatus::DOOR_CLOSING))
    {
        PlayErrorBeep();
        ShowWarning("Warning Bay Doors in Motion Radiator is Locked.wav", DeltaGliderXR1::ST_WarningCallout, "Cannot deploy/retract radiator&while bay doors are in motion.");
        return;  // cannot move
    }

    // cannot deploy or retract radiator if bay doors are OPEN (they would collide)
    if (bay_status == DoorStatus::DOOR_OPEN)
    {
        PlayErrorBeep();
        ShowWarning("Warning Bay Doors Open Radiator is Locked.wav", DeltaGliderXR1::ST_WarningCallout, "Cannot deploy/retract radiator&while bay doors are open.");
        return;  // cannot move
    }

    bool close = (action == DoorStatus::DOOR_CLOSED || action == DoorStatus::DOOR_CLOSING);
    radiator_status = action;

    CHECK_DOOR_JUMP(radiator_proc, anim_radiator);

    TriggerRedrawArea(AID_RADIATORSWITCH);
    TriggerRedrawArea(AID_RADIATORINDICATOR);

    UpdateCtrlDialog(this);
    RecordEvent ("RADIATOR", close ? "CLOSE" : "OPEN");
}

// prevent landing gear from being raised if the gear is not yet fully uncompressed
void XR3Phoenix::ActivateLandingGear(DoorStatus action)
{
    if (((action == DoorStatus::DOOR_OPENING) || (action == DoorStatus::DOOR_CLOSING)) && ((m_noseGearProc != 1.0) || (m_rearGearProc != 1.0)))
    {
        PlayErrorBeep();
        ShowWarning("Gear Locked.wav", DeltaGliderXR1::ST_WarningCallout, "Gear is still in contact with the&ground: cannot raise landing gear.");
        return;
    }

    // propogate to the superclass
    DeltaGliderXR1::ActivateLandingGear(action);
}

// Used for internal development testing only to tweak some internal value.
// This is invoked from the key handler as ALT-1 or ALT-2 are held down.  
// direction = true: increment value, false: decrement value
void XR3Phoenix::TweakInternalValue(bool direction)
{
#ifdef _DEBUG  // debug only!
#if 0
    const double stepSize = (oapiGetSimStep() * ELEVATOR_TRIM_SPEED * 0.02);
    
    // tweak hidden elevator trim to fix nose-up push
    double step = stepSize * (direction ? 1.0 : -1.0);
    m_hiddenElevatorTrimState += step;

    if (m_hiddenElevatorTrimState < -1.0)
        m_hiddenElevatorTrimState = -1.0;
    else if (m_hiddenElevatorTrimState > 1.0)
        m_hiddenElevatorTrimState = 1.0;

    SetControlSurfaceLevel(AIRCTRL_FLAP, m_hiddenElevatorTrimState);     
    sprintf(oapiDebugString(), "Hidden trim=%lf", m_hiddenElevatorTrimState);
#endif
#if 0
    const double stepSize = (oapiGetSimStep() * 0.05);  // 20 seconds to go from 0...1
    m_tweakedInternalValue += (direction ? stepSize : -stepSize);
    sprintf(oapiDebugString(), "tweakedInternalValue=%lf", m_tweakedInternalValue);
#endif
#endif
}

// Render hatch decompression exhaust stream
void XR3Phoenix::ShowHatchDecompression()
{
    // NOTE: I assume this structure is actually treated as CONST by the Orbiter core, since the animation 
    // structures not declared CONST either.
    static PARTICLESTREAMSPEC airvent = {
        0, 1.0, 15, 0.5, 0.3, 2, 0.3, 1.0, PARTICLESTREAMSPEC::EMISSIVE,
        PARTICLESTREAMSPEC::LVL_LIN, 0.1, 0.1,
        PARTICLESTREAMSPEC::ATM_FLAT, 0.1, 0.1
    };
    /* Postions are:
         NOSE
          
         1  2

         3  4
    */
    static const VECTOR3 pos[4] = 
    {
        {-1.824, 6.285, 18.504},   // left-front
        { 1.824, 6.285, 18.504},   // right-front
        {-2.158, 7.838, 5.292},    // left-rear
        { 2.158, 7.838, 5.292}     // right-rear
    };

    static const VECTOR3 dir[4] = 
    {
        {-0.802,  0.597, 0},
        { 0.802,  0.597, 0},  
        {-0.050,  0.988, 0},
        { 0.050,  0.988, 0}
    };

    hatch_vent = new PSTREAM_HANDLE[4];   // this will be freed automatically for us later
    hatch_venting_lvl = new double[4];    // ditto
    for (int i=0; i < 4; i++)
    {
        hatch_venting_lvl[i] = 0.4;
        hatch_vent[i] = AddParticleStream(&airvent, pos[i], dir[i], hatch_venting_lvl + i);
    }

    hatch_vent_t = GetAbsoluteSimTime();
}

// turn off hatch decompression exhaust stream; invoked form a PostStep
void XR3Phoenix::CleanUpHatchDecompression()
{
    for (int i=0; i < 4; i++)
        DelExhaustStream(hatch_vent[i]);
}

// Define the active airlock for MMu as set in the m_activeEVAPort member variable; this is invoked each time the active EVA port is changed.
void XR3Phoenix::DefineMmuAirlock()
{
    switch (m_activeEVAPort)
    {
    case ACTIVE_EVA_PORT::DOCKING_PORT:
        {
            const float airlockY = static_cast<float>(DOCKING_PORT_COORD.y);
            const float airlockZ = static_cast<float>(DOCKING_PORT_COORD.z);

#ifdef MMU
            //                      state,MinX, MaxX, MinY,             MaxY,             MinZ,             MaxZ
            UMmu.DefineAirLockShape(1, -0.66f, 0.66f, airlockY - 3.00f, airlockY + 0.20f, airlockZ - 0.66f, airlockZ + 0.66f);  
            VECTOR3 pos = { 0.0, airlockY + 2.0, airlockZ }; // this is the position where the Mmu will appear relative to the ship's local coordinates
            VECTOR3 rot = { 0.0, 0.0, 0.0 };  // straight up, facing forward
            UMmu.SetMembersPosRotOnEVA(pos, rot);
            UMmu.SetEjectPosRotRelSpeed(pos, rot, _V(0, 4.0, 0));  // jumped UP to bail out @ 4 meters-per-second
            UMmu.SetActiveDockForTransfer(0);       // ship-to-ship transfer enabled
#endif
            m_pActiveAirlockDoorStatus = &olock_status;
            break;
        }

    case ACTIVE_EVA_PORT::CREW_ELEVATOR:
        {
            // PRE-1.3 RC2: Port location (deployed): -3.116, -9.092, 6.35 
            // NEW: Port location (deployed):         -3.116 + 0.7, -9.092 - 0.7, 6.35 
            // add X 0.6 and Y 0.7 here for post-1.3 RC2 coordinates
            const float airlockX = -3.116f - 0.6f;
            const float airlockY = -7.299f + 0.7f;   // position coordinates refer to the TOP of the astronaut, so we have to allow space along the Y axis

            const float airlockZ =  6.35f; 
            const float xDim = 4.692f / 2;   // width from center
            const float yDim = 2.772f / 2;   // height from center
            const float zDim = 3.711f / 2;   // depth from center
#ifdef MMU
            //                      state,MinX,          MaxX,            MinY,           MaxY,            MinZ,            MaxZ
            UMmu.DefineAirLockShape(1, airlockX - xDim, airlockX + xDim, airlockY - yDim, airlockY + yDim, airlockZ - zDim, airlockZ + zDim);  
            VECTOR3 pos = { airlockX, airlockY, airlockZ + zDim + 1.0}; // this is the position where the Mmu will appear relative to the ship's local coordinates
            VECTOR3 rot = { 0.0, 0.0, 0.0 };  // straight up, facing forward
            UMmu.SetMembersPosRotOnEVA(pos, rot);
            UMmu.SetEjectPosRotRelSpeed(pos, rot, _V(0, -2.0, 0));  // jumped DOWN to bail out @ 2 meters-per-second
            UMmu.SetActiveDockForTransfer(-1);       // ship-to-ship transfer disabled
#endif
            m_pActiveAirlockDoorStatus = &crewElevator_status;
            break;
        }

        // NOTE: default case should never happen!
    }

#ifdef MMU
    // UMmu bug: must set this every time we reset the docking port AFTER the port is defined!
    UMmu.SetMaxSeatAvailableInShip(MAX_PASSENGERS); // includes the pilot
    UMmu.SetCrewWeightUpdateShipWeightAutomatically(FALSE);  // we handle crew member weight ourselves
#endif
    // repaint both LEDs and the switch
    TriggerRedrawArea(AID_EVA_DOCKING_PORT_ACTIVE_LED); 
    TriggerRedrawArea(AID_EVA_CREW_ELEVATOR_ACTIVE_LED); 
    TriggerRedrawArea(AID_ACTIVE_EVA_PORT_SWITCH); 
}

// returns: true if EVA doors are OK, false if not
bool XR3Phoenix::CheckEVADoor()
{
    if (m_activeEVAPort == ACTIVE_EVA_PORT::DOCKING_PORT)
        return DeltaGliderXR1::CheckEVADoor();

    // else it's the crew elevator
    // NOTE: if the gear has collapsed, cannot EVA via the elevator!  Also note that we cannot use GetGearFullyCompressedAltitude here, since that will be 0
    // even after gear collapse since GroundContact will be true.
    if ((crewElevator_status == DoorStatus::DOOR_FAILED) || (GetAltitude(ALTMODE_GROUND) < (GEAR_FULLY_COMPRESSED_DISTANCE-0.2)))  // leave a 0.2-meter safety cushion
    {
        PlayErrorBeep();
        ShowWarning("Warning Elevator Failure.wav", DeltaGliderXR1::ST_WarningCallout, "Crew Elevator is damanged.");
        return false;
    }
    else if (crewElevator_status != DoorStatus::DOOR_OPEN)
    {
        PlayErrorBeep();
        ShowWarning("Warning Elevator is Closed.wav", DeltaGliderXR1::ST_WarningCallout, "Crew Elevator is stowed.");
        return false;
    }

    return true;
}

// Set the camera to its default payload bay postion.
void XR3Phoenix::ResetCameraToPayloadBay()
{
    /* ORG
    const VECTOR3 pos = { 0, 7.755, 4.077 };
    const VECTOR3 dir = { 0.0, -0.297, -0.955 };  // look down to rear bottom of bay
    */

    // PRE-1.7: const VECTOR3 pos = { 0, 8.755, 4.077 };
    const VECTOR3 pos = { 0, 8.755 + 1.0, 4.077 };  // so we don't clip under the D3D9 client
    const VECTOR3 dir = { 0.0, -0.297, -0.955 };  // look down to rear bottom of bay

    SetCameraOffset(pos);
    SetXRCameraDirection(dir); 
}

// override clbkPanelRedrawEvent so we can limit our refresh rates for our custom screens
bool XR3Phoenix::clbkPanelRedrawEvent(int areaID, int event, SURFHANDLE surf)
{
    const XR3ConfigFileParser *pConfig = GetXR3Config();

    // Only filter PANEL_REDRAW_ALWAYS events for timing!
    if (event == PANEL_REDRAW_ALWAYS)
    {
        // NOTE: we want to check *realtime* deltas, not *simulation time* here: repaint frequency should not
        // vary based on time acceleration.
        const double uptime = GetSystemUptime();  // will always count up

        // check for area IDs that have custom refresh rates
        int screenIndex = 0;  // for payload screens
        switch (areaID)
        {
        case AID_SELECT_PAYLOAD_BAY_SLOT_SCREEN:
            goto check;     // 0

        case AID_GRAPPLE_PAYLOAD_SCREEN:
            screenIndex = 1;
            goto check;

        case AID_DEPLOY_PAYLOAD_SCREEN:
            screenIndex = 2;
check:
            if (uptime < m_nextPayloadScreensRefresh[screenIndex])
                return false;

            // update for next interval
            m_nextPayloadScreensRefresh[screenIndex] = uptime + pConfig->PayloadScreensUpdateInterval;

            // force the repaint to occur by invoking the VESSEL3 superclass directly; otherwise the XR1 impl would 
            // see each of these areas as just another area and limit it by PanelUpdateInterval yet, which we want to bypass.
            return VESSEL3_EXT::clbkPanelRedrawEvent(areaID, event, surf);
        }
    }

    // redraw is OK: invoke the superclass to dispatch the redraw event
    return DeltaGliderXR1::clbkPanelRedrawEvent(areaID, event, surf);
}


// Returns max configured thrust for the specified thruster BEFORE taking atmosphere or 
// damage into account.
// index = 0-13
double XR3Phoenix::GetRCSThrustMax(const int index) const
{
    // obtain the "normal" RCS jet power from the superclass
    double rcsThrustMax = DeltaGliderXR1::GetRCSThrustMax(index);

    // if holding attitude, adjust RCS max thrust based on payload in the bay
    if (InAtm())
    {
        if ((m_customAutopilotMode == AUTOPILOT::AP_ATTITUDEHOLD) || (m_customAutopilotMode == AUTOPILOT::AP_DESCENTHOLD))
        {
            const double withPayloadMass = GetEmptyMass();        // includes payload
            const double payloadMass = GetPayloadMass();
            const double noPayloadMass = withPayloadMass - payloadMass;  // total mass without any payload
            const double multiplier = withPayloadMass / noPayloadMass;   // 1.0 = no payload, etc.
            rcsThrustMax *= multiplier;
            // DEBUG: sprintf(oapiDebugString(), "RCS multiplier=%lf", multiplier);
        }
    }

    return rcsThrustMax;
}

// --------------------------------------------------------------
// Apply custom skin to the current mesh instance
// --------------------------------------------------------------
void XR3Phoenix::ApplySkin()
{
    if (!exmesh) return;
    
    if (skin[0])    // XR3t.dds
	{
		oapiSetTexture(exmesh, 1, skin[0]);
        oapiSetTexture(exmesh, 4, skin[0]);
	}

    if (skin[1])     // XR3b.dds
	{
		oapiSetTexture(exmesh,  2, skin[1]);
        oapiSetTexture(exmesh, 17, skin[1]);
	}
}

// meshTextureID = vessel-specific constant that is translated to a texture index specific to our vessel's .msh file.  meshTextureID 
// NOTE: meshTextureID=VCPANEL_TEXTURE_NONE = -1 = "no texture" (i.e., "not applicable"); defined in Area.h.
// hMesh = OUTPUT: will be set to the mesh handle of the mesh associated with meshTextureID.
DWORD XR3Phoenix::MeshTextureIDToTextureIndex(const int meshTextureID, MESHHANDLE &hMesh)
{
    _ASSERTE(false);  // should never reach here!

    hMesh = nullptr;      
    return MAXDWORD;   // bogus
}