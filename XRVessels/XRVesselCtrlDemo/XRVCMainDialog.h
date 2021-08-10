//-------------------------------------------------------------------------
// XRVCMainDialog.h : Class definition for singleton XRVesselCtrlDemo main dialog.
//
// Copyright 2010-2018 Douglas E. Beachy; All Rights Reserved.
//
// This software is FREEWARE and may not be sold!
//
// NOTE: You may not redistribute this file nor use it in any other project without
// express consent from the author.  
//
// http://www.alteaaerospace.com
// mailto:dbeachy@speakeasy.net
//-------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <atlstr.h>  // we use CString instead of std::string primarily because we want the CString.Format method
#include "resource.h"

#include "XRVCClientCommandParser.h"
#include "XRVCClient.h"
#include "XRVCScriptThread.h"

#define VERSION "XRVesselCtrlDemo 3.1"

class XRVCMainDialog
{
public:
    // public member methods
    XRVCMainDialog(const HINSTANCE hDLL);  
    virtual ~XRVCMainDialog();
    
    static void OpenDialogClbk(void *pContext);

    // Note: for simplicity, this class is designed to be a singleton since that is all we need for Orbiter.
    // None of the the other classes in the project, however, are limited by design to a single instance.
    static XRVCMainDialog *s_pSingleton;    // references our singleton main dialog object

    // static state data saved in/loaded from the scenario file
    static bool s_enableFullScreenMode;

   // this method must be public so we can call it from a leaf handler in the parser
    bool ExecuteScriptFile(const char *pFilename) { return m_pScriptThread->OpenScriptFile(pFilename); } 

protected:
    // identifies text panels on the dialog
    enum TextPanel { TEXTPANEL_LEFT, TEXTPANEL_RIGHT, TEXTPANEL_BOTH };

    static BOOL CALLBACK MsgProcMain         (const HWND hDlg, const UINT uMsg, const WPARAM wParam, const LPARAM lParam);
    static BOOL CALLBACK MsgProcHelp         (const HWND hDlg, const UINT uMsg, const WPARAM wParam, const LPARAM lParam);
    static LRESULT CALLBACK CommandBoxMsgProc(const HWND hWnd, const UINT uMsg, const WPARAM wParam, const LPARAM lParam);

    static const char *GetComboLineForVessel(const VESSEL *pVessel);

    void Close() { oapiCloseDialog(m_hwndDlg); }
    void ErrorBeep() const { MessageBeep(MB_ICONASTERISK); }
    void AutocompleteBeep() const { MessageBeep(MB_OK); }
    
    void RefreshVesselList();
    void SelectFocusVessel() const;
    void ComboVesselChanged();
    void RefreshDataSection();
    bool HandleExecuteScript();
    const char *GetSelectedVesselName() const;
    void SetFocusToSelectedVessel() const;
    
    void EnsureLeftRightModesSet();
    void UncheckAllModeButtons(TextPanel panelID);

    void ProcessModeSwitchLeft(const int buttonIDC);
    void ProcessModeSwitchRight(const int buttonIDC);

    int GetActiveModeLeftIDC() const;  // IDC_CHECK_MAIN, IDC_CHECK_RETRO, etc.
    int GetActiveModeRightIDC() const; // IDC_CHECK_STATUS, IDC_CHECK_DOORS, etc.

    bool CheckXRVesselForCommand();
    bool ProcessCommandKeystroke(const WPARAM keycode, const UINT uMsg);
    
    bool ExecuteCommand();  // read command from GUI and execute
    bool ExecuteCommand(CString &csCommand);  // autocomplete & execute the supplied command
    bool ExecuteScriptFile() { return m_pScriptThread->OpenScriptFile(); }
    void ToggleHelp();
    void ToggleFullScreenMode() { s_enableFullScreenMode = !s_enableFullScreenMode; UpdateFromStaticFields(); }
    void UpdateFromStaticFields();
    bool AutoCompleteCommand(const bool direction);
    
    void GetCommandText(CString &csOut) const;
    void SetCommandText(const char *pNewText) const;
    void SetStatusText(const char *pNewText) const { SetWindowTextSmart(GetDlgItem(m_hwndDlg, IDC_STATUSBOX), pNewText); }

    HFONT GetFontForMode(const int modeIDC) const;
    void XRStatusOut(const int editBoxOutIDC, const int modeIDC);
    void RemoveLastTokenFromCommandLine();
    void UpdateAvailableParams() const;
    void EnableDisableButtons() const;
    bool SetWindowTextSmart(HWND hWnd, const char *pString) const;
    void clbkHelpWindowClosed() { m_hwndHelpDlg = 0; }  // invoked when our help window closes itself
    void CloseHelpWindow() {  if (m_hwndHelpDlg != 0) { oapiCloseDialog(m_hwndHelpDlg); m_hwndHelpDlg = nullptr; } }

    // only used for debugging
    bool DumpCommandTree(const char *pFilename);
    void BuildCommandHelpTree(CString &csOut) { m_pxrvcClientCommandParser->BuildCommandHelpTree(csOut); }

    // data
    HWND m_hwndDlg;    // our singleton main dialog handle
    HINSTANCE m_hDLL;  // our DLL handle
    CString m_csLeftPanelText;
    CString m_csRightPanelText;
    HWND m_hwndHelpDlg;  // our help dialog
    XRVCScriptThread *m_pScriptThread;  // handles script parsing for us
    
    static void *s_pCommandBoxOldMessageProc; 
    XRVCClient m_xrvcClient;   // handles XRVesselCtrl interface calls
    XRVCClientCommandParser *m_pxrvcClientCommandParser;
    HFONT m_hCourierFontSmall;   
    HFONT m_hCourierFontNormal;   

    // control ID groups
    const static int MODE_GROUP_LEFT_IDCs[4];  // Main, Retro, etc.
    const static int MODE_GROUP_RIGHT_IDCs[4]; // Status, Doors, etc.

#define MODE_GROUP_LEFT_COUNT (sizeof(MODE_GROUP_LEFT_IDCs) / sizeof(int))
#define MODE_GROUP_RIGHT_COUNT (sizeof(MODE_GROUP_RIGHT_IDCs) / sizeof(int))
#define TIMERID_20_TICKS_A_SECOND    1
#define TIMERID_UDPATE_AVAILABLE_PARAMS 2
};