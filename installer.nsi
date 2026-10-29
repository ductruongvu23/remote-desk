; Bộ cài Remote Desk cho PC (NSIS 3, gói mingw-w64-ucrt-x86_64-nsis của MSYS2).
;   bash build_host.sh && makensis -DVERSION=1.1.0 installer.nsi   ->  RemoteDesk-Setup-1.1.0.exe
; Cài cho người dùng hiện tại, không cần quyền quản trị, vào %LOCALAPPDATA%\Programs\Remote Desk:
; app ghi tài khoản, khoá thiết bị và cài đặt cạnh file exe nên không cài vào Program Files (chỉ đọc).
; File phải lưu UTF-8 có BOM: không có BOM thì makensis đọc theo bảng mã ANSI, chữ tiếng Việt vỡ.
Unicode true
!include MUI2.nsh

!ifndef VERSION
  !define VERSION "dev"
!endif
!define APP "Remote Desk"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\RemoteDesk"
!define RUNKEY "Software\Microsoft\Windows\CurrentVersion\Run"

Name "${APP}"
OutFile "RemoteDesk-Setup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "${UNINST}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ManifestDPIAware true
BrandingText "${APP} ${VERSION}"

!define MUI_ICON "icons\app_icon.ico"
!define MUI_UNICON "icons\app_icon.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_NODESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\remote_host.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Mở Remote Desk"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Vietnamese"

; Đóng Remote Desk đang chạy bằng WM_CLOSE (app tự đóng cổng router khi thoát) để ghi đè/xoá được file exe.
!macro CLOSE_APP un
Function ${un}CloseApp
  StrCpy $1 0
  loop:
    FindWindow $0 "RemoteDeskHost"
    IntCmp $0 0 done
    SendMessage $0 ${WM_CLOSE} 0 0 /TIMEOUT=2000
    Sleep 300
    IntOp $1 $1 + 1
    IntCmp $1 20 done loop done
  done:
FunctionEnd
!macroend
!insertmacro CLOSE_APP ""
!insertmacro CLOSE_APP "un."

Section "Remote Desk (bắt buộc)" SecMain
  SectionIn RO
  Call CloseApp
  SetOutPath "$INSTDIR"
  File "remote_host.exe"
  File "remote_desktop.html"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\remote_host.exe"
  ; cài lại thì theo lựa chọn mới: hai mục tuỳ chọn bên dưới tự thêm lại nếu được chọn
  Delete "$DESKTOP\${APP}.lnk"
  DeleteRegValue HKCU "${RUNKEY}" "RemoteDesk"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\remote_host.exe"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Section "Lối tắt trên màn hình nền" SecDesktop
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\remote_host.exe"
SectionEnd

; Tắt sẵn: chỉ cần khi muốn điều khiển PC từ xa mà không phải tự mở app sau mỗi lần bật máy
Section /o "Tự mở khi đăng nhập Windows" SecAutostart
  WriteRegStr HKCU "${RUNKEY}" "RemoteDesk" '"$INSTDIR\remote_host.exe"'
SectionEnd

Section "Uninstall"
  Call un.CloseApp
  Delete "$INSTDIR\remote_host.exe"
  Delete "$INSTDIR\remote_desktop.html"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  DeleteRegValue HKCU "${RUNKEY}" "RemoteDesk"
  DeleteRegKey HKCU "${UNINST}"
  ; Dữ liệu người dùng giữ lại trừ khi được đồng ý xoá (gỡ im lặng /S thì giữ)
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 \
    "Xoá luôn tài khoản, các iPhone đã ghép và cài đặt của Remote Desk trên PC này?" /SD IDNO IDNO keep
    Delete "$INSTDIR\remote_account.txt"
    Delete "$INSTDIR\remote_devices.txt"
    Delete "$INSTDIR\remote_settings.ini"
    Delete "$INSTDIR\remote_keymap.json"
    Delete "$INSTDIR\remote_pin.txt"
  keep:
  RMDir "$INSTDIR"  ; chỉ xoá khi thư mục đã trống
SectionEnd
