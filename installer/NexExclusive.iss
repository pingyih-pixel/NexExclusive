; NexExclusive 安装程序（Inno Setup 6）。由 release.sh 调用，不要手工改版本号：
;   ISCC /DAppVersion=1.2.3 /DPackageDir=<dist\NexExclusive-1.2.3> /DOutputDir=<dist> NexExclusive.iss
;
; 安装逻辑只有一份，在 NexExclusive.exe --install 里（mod/tools/host_install.inc）：
; 校验包内清单、停掉旧宿主和客户端、原子替换安装目录、写卸载项 / 自启 / 开始菜单。
; 这个安装程序只负责把便携包解到临时目录，再调用那条命令。所以它自己不注册
; 卸载程序（Uninstallable=no）——否则「设置 → 应用」里会出现两个 NexExclusive。

#ifndef AppVersion
  #error 缺少 AppVersion：请用 release.sh 构建，或传 /DAppVersion=1.2.3
#endif
#ifndef PackageDir
  #error 缺少 PackageDir：指向 release.sh 生成的 dist\NexExclusive-<版本> 目录
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define ZhIsl AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl"

[Setup]
AppId=NexExclusive
AppName=NexExclusive
AppVersion={#AppVersion}
AppVerName=NexExclusive {#AppVersion}
AppPublisher=NeteaseExclusive
AppPublisherURL=https://github.com/pingyih-pixel/NexExclusive
VersionInfoVersion={#AppVersion}
VersionInfoProductName=NexExclusive
VersionInfoDescription=NexExclusive Setup
; 真正的安装目录由 --install 决定（%LOCALAPPDATA%\NexExclusive），这里只是占位
DefaultDirName={localappdata}\NexExclusive
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=NexExclusive-{#AppVersion}-setup
SetupIconFile=..\mod\tools\nexhost.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
; 旧宿主和客户端由 --install 自己停，不让 Inno 的重启管理器插手
CloseApplications=no
RestartApplications=no

[Languages]
#if FileExists(ZhIsl)
Name: "zh"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#else
Name: "en"; MessagesFile: "compiler:Default.isl"
#endif

[Files]
; 整个便携包（含 manifest.txt，--install 会按它校验每个文件的哈希）放进临时目录
Source: "{#PackageDir}\*"; DestDir: "{tmp}\pkg"; Flags: recursesubdirs createallsubdirs ignoreversion

[Run]
Filename: "{localappdata}\NexExclusive\NexExclusive.exe"; WorkingDir: "{localappdata}\NexExclusive"; \
  Description: "启动 NexExclusive"; Flags: postinstall nowait skipifsilent; Check: InstallSucceeded

[Code]
var
  BuiltInInstallOk: Boolean;

function InstallSucceeded: Boolean;
begin
  Result := BuiltInInstallOk;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result :=
    '安装位置：' + NewLine + Space + ExpandConstant('{localappdata}\NexExclusive') + NewLine + NewLine +
    '安装时会：' + NewLine +
    Space + '· 关闭正在运行的网易云音乐和 NexExclusive' + NewLine +
    Space + '· 登记开机自启、开始菜单快捷方式和「设置 → 应用」里的卸载项' + NewLine + NewLine +
    'Windows Defender 可能把 NexExclusive.exe 误报为 Behavior:Win32/DefenseEvasion.A!ml' + NewLine +
    '并隔离。处理方法见包内 README.txt 的「杀软报毒」一节。';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Params, LogPath, KeptLog: String;
begin
  if CurStep <> ssPostInstall then Exit;
  LogPath := ExpandConstant('{tmp}\install-output.txt');
  { 经 cmd 把输出重定向到文件：NexExclusive.exe 在没有标准输出时会自己开一个
    控制台窗口；有了文件句柄就不会弹窗，失败时还能把原因拿给用户看。 }
  Params := '/C ""' + ExpandConstant('{tmp}\pkg\NexExclusive.exe') + '" --install > "' +
            LogPath + '" 2>&1"';
  BuiltInInstallOk :=
    Exec(ExpandConstant('{cmd}'), Params, ExpandConstant('{tmp}\pkg'), SW_HIDE,
         ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  Log('NexExclusive.exe --install exit code ' + IntToStr(ResultCode));
  { 输出是 UTF-8，不在这里解码显示；在临时目录之外留一份，出问题时可以打开看。 }
  KeptLog := ExpandConstant('{localappdata}\NexExclusive-setup.log');
  FileCopy(LogPath, KeptLog, False);
  if not BuiltInInstallOk then
    SuppressibleMsgBox('安装没有完成（NexExclusive.exe --install 退出码 ' + IntToStr(ResultCode) + '）。' +
      #13#10 + '原有安装保持不变。' + #13#10#13#10 +
      '详细输出：' + KeptLog + #13#10#13#10 +
      '如果文件被杀软隔离，请看便携包 README.txt 的「杀软报毒」一节。',
      mbCriticalError, MB_OK, IDOK);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and not BuiltInInstallOk then
    WizardForm.FinishedLabel.Caption :=
      'NexExclusive 没有装上，原有安装（如果有）保持不变。' + #13#10#13#10 +
      '可以改用便携版：解压 zip 后直接运行 NexExclusive.exe。';
end;
